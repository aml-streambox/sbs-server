/*
 * SBS - StreamBox Broadcast System
 * Output Supervisor — fork/exec output workers, send composed frames, crash recovery
 *
 * The supervisor runs on the main thread (GLib main loop) and:
 *   1. Creates a Unix socket for each output worker
 *   2. Fork/execs sbs-worker --mode=output, pipes JSON config to stdin
 *   3. Accepts the worker connection
 *   4. Sends composed frames via sendmsg (SCM_RIGHTS for DMA-BUF fds)
 *   5. Receives STATUS heartbeats from workers via GLib I/O watch
 *   6. Monitors worker health via SIGCHLD and heartbeat timeouts
 *   7. Restarts crashed workers with exponential backoff
 *   8. Implements 4-stage graceful shutdown
 *
 * Reference: document/06-output-manager.md sections 2, 6-8
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "out-sup"

#include "sbs/output_supervisor.h"
#include "sbs/ipc.h"
#include "sbs/ipc_transport.h"
#include "sbs/log.h"

#include <cjson/cJSON.h>
#include <glib-unix.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

/* ── Constants ────────────────────────────────────────────────── */

#define BACKOFF_BASE_MS         1000     /* 1 second base backoff */
#define BACKOFF_MAX_MS          30000    /* 30 seconds max backoff */
#define MAX_RESTART_ATTEMPTS    10       /* Give up after 10 consecutive failures */
#define STABLE_RESET_MS         60000    /* Reset backoff counter after 60s stable */
#define SOCKET_RCVBUF_SIZE      262144   /* 256 KB receive buffer */
#define SOCKET_SNDBUF_SIZE      262144   /* 256 KB send buffer */
#define ACCEPT_TIMEOUT_MS       5000     /* 5 second timeout for worker to connect */
#define SHUTDOWN_GRACE_MS       2000     /* Stage 0+1: 2 seconds for clean exit */
#define SHUTDOWN_SIGTERM_MS     1000     /* Stage 2: 1 second after SIGTERM */

/* ── Output Entry (per-worker state) ──────────────────────────── */

typedef struct sbs_output_entry {
    /* Identity */
    char        *output_id;
    char        *socket_path;

    /* Worker process */
    GPid         pid;
    int          listen_fd;       /* Listening socket fd */
    int          conn_fd;         /* Accepted connection fd (-1 if not connected) */

    /* GLib watch IDs (0 = not installed) */
    guint        io_watch_id;     /* I/O watch on conn_fd (for STATUS messages) */
    guint        child_watch_id;  /* SIGCHLD watch on pid */
    guint        accept_timer_id; /* Timer for accept timeout */
    guint        stable_timer_id; /* Timer to reset backoff after stable operation */
    guint        restart_timer_id;/* Timer for scheduled restart with backoff */

    /* Health & recovery */
    uint32_t     restart_attempts;  /* Consecutive failures since last stable period */
    int64_t      last_start_time;   /* g_get_monotonic_time() when last started */
    bool         shutting_down;     /* Set when stop requested */
    bool         connected;         /* Worker connected and ready to receive frames */

    /* Metrics */
    uint64_t     frames_sent;
    uint64_t     frames_dropped;    /* Send failed (WOULD_BLOCK) */

    /* Configuration (stored for restart) */
    sbs_output_start_config_t config_copy;

    /* Deep-copied config strings (freed on entry destruction) */
    char        *cfg_codec;
    char        *cfg_encoder;
    char        *cfg_sink_type;
    char        *cfg_srt_uri;
    char        *cfg_srt_mode;
    char        *cfg_srt_stream_key;
    char        *cfg_rtmp_uri;
    char        *cfg_rtmp_passcode;
    char        *cfg_rtmp_plugin;
    char        *cfg_file_path;
    char        *cfg_file_path_mode;
    char        *cfg_file_prefix;
    char        *cfg_file_container;

    /* Back-reference to supervisor */
    struct sbs_output_supervisor *supervisor;
} sbs_output_entry_t;

/* ── Supervisor ───────────────────────────────────────────────── */

struct sbs_output_supervisor {
    GHashTable  *outputs;      /* output_id → sbs_output_entry_t* */
    char        *worker_path;  /* Path to sbs-worker binary */
    char        *sock_dir;     /* Directory for IPC sockets */
};

/* ── Forward Declarations ─────────────────────────────────────── */

static void     output_entry_free(sbs_output_entry_t *entry);
static int      output_entry_launch(sbs_output_entry_t *entry);
static void     output_entry_cleanup_worker(sbs_output_entry_t *entry);
static char    *build_config_json(const sbs_output_entry_t *entry);
static gboolean on_worker_data(gint fd, GIOCondition cond, gpointer user_data);
static void     on_child_exit(GPid pid, gint status, gpointer user_data);
static gboolean on_accept_timeout(gpointer user_data);
static gboolean on_stable_timer(gpointer user_data);
static void     schedule_restart(sbs_output_entry_t *entry);
static gboolean on_restart_timer(gpointer user_data);

/* ── Supervisor Lifecycle ─────────────────────────────────────── */

sbs_output_supervisor_t *sbs_output_supervisor_new(const char *worker_path,
                                                    const char *sock_dir)
{
    sbs_output_supervisor_t *sup = calloc(1, sizeof(*sup));
    if (!sup) return NULL;

    sup->outputs     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, /* key is owned by entry */
                                              (GDestroyNotify)output_entry_free);
    sup->worker_path = strdup(worker_path);
    sup->sock_dir    = strdup(sock_dir);

    if (!sup->outputs || !sup->worker_path || !sup->sock_dir) {
        sbs_output_supervisor_free(sup);
        return NULL;
    }

    LOG_I("output supervisor created (worker=%s, sock_dir=%s)",
          worker_path, sock_dir);
    return sup;
}

void sbs_output_supervisor_free(sbs_output_supervisor_t *sup)
{
    if (!sup) return;

    if (sup->outputs) {
        /* This will call output_entry_free for each entry */
        g_hash_table_destroy(sup->outputs);
    }
    free(sup->worker_path);
    free(sup->sock_dir);
    free(sup);
}

uint32_t sbs_output_supervisor_output_count(const sbs_output_supervisor_t *sup)
{
    GHashTableIter iter;
    gpointer value;
    uint32_t count = 0;

    if (!sup || !sup->outputs) return 0;

    g_hash_table_iter_init(&iter, sup->outputs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_output_entry_t *entry = value;
        if (entry && entry->connected && !entry->shutting_down && entry->conn_fd >= 0) {
            count++;
        }
    }
    return count;
}

void sbs_output_supervisor_get_metrics(const sbs_output_supervisor_t *sup,
                                       sbs_output_supervisor_metrics_t *metrics)
{
    GHashTableIter iter;
    gpointer value;

    if (!metrics) {
        return;
    }

    memset(metrics, 0, sizeof(*metrics));
    if (!sup || !sup->outputs) {
        return;
    }

    g_hash_table_iter_init(&iter, sup->outputs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const sbs_output_entry_t *entry = value;
        bool connected = entry->pid > 0 && entry->connected && entry->conn_fd >= 0;
        bool restarting = !entry->shutting_down && entry->restart_attempts > 0 && !connected;

        metrics->total_outputs++;
        if (connected) {
            metrics->connected_outputs++;
        }
        if (restarting) {
            metrics->restarting_outputs++;
        }
        if (entry->restart_attempts > metrics->max_restart_attempts) {
            metrics->max_restart_attempts = entry->restart_attempts;
        }
        metrics->frames_sent += entry->frames_sent;
        metrics->frames_dropped += entry->frames_dropped;
    }

    metrics->degraded = metrics->restarting_outputs > 0;
}

/* ── Start Output ─────────────────────────────────────────────── */

int sbs_output_supervisor_start_output(sbs_output_supervisor_t *sup,
                                        const sbs_output_start_config_t *config)
{
    if (!sup || !config || !config->output_id) {
        return SBS_ERR_INVAL;
    }

    /* Check for duplicate */
    if (g_hash_table_contains(sup->outputs, config->output_id)) {
        LOG_W("output '%s' already exists", config->output_id);
        return SBS_ERR_INVAL;
    }

    /* Create entry */
    sbs_output_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return SBS_ERR_NOMEM;

    entry->output_id  = strdup(config->output_id);
    entry->listen_fd  = -1;
    entry->conn_fd    = -1;
    entry->pid        = 0;
    entry->connected  = false;
    entry->supervisor = sup;

    /* Build socket path */
    size_t path_len = strlen(sup->sock_dir) + strlen(config->output_id) + 16;
    entry->socket_path = malloc(path_len);
    if (!entry->socket_path) {
        output_entry_free(entry);
        return SBS_ERR_NOMEM;
    }
    snprintf(entry->socket_path, path_len, "%s/output-%s.sock",
             sup->sock_dir, config->output_id);

    /* Deep-copy config for restarts */
    entry->config_copy = *config;
    entry->config_copy.output_id = entry->output_id;  /* use already duped */

    /* Deep-copy string fields */
    entry->cfg_codec     = config->codec ? strdup(config->codec) : NULL;
    entry->cfg_encoder   = config->encoder ? strdup(config->encoder) : NULL;
    entry->cfg_sink_type = config->sink_type ? strdup(config->sink_type) : NULL;
    entry->cfg_srt_uri   = config->srt_uri ? strdup(config->srt_uri) : NULL;
    entry->cfg_srt_mode  = config->srt_mode ? strdup(config->srt_mode) : NULL;
    entry->cfg_srt_stream_key = config->srt_stream_key ? strdup(config->srt_stream_key) : NULL;
    entry->cfg_rtmp_uri  = config->rtmp_uri ? strdup(config->rtmp_uri) : NULL;
    entry->cfg_rtmp_passcode = config->rtmp_passcode ? strdup(config->rtmp_passcode) : NULL;
    entry->cfg_rtmp_plugin = config->rtmp_plugin ? strdup(config->rtmp_plugin) : NULL;
    entry->cfg_file_path = config->file_path ? strdup(config->file_path) : NULL;
    entry->cfg_file_path_mode = config->file_path_mode ? strdup(config->file_path_mode) : NULL;
    entry->cfg_file_prefix = config->file_prefix ? strdup(config->file_prefix) : NULL;
    entry->cfg_file_container = config->file_container ? strdup(config->file_container) : NULL;

    entry->config_copy.codec     = entry->cfg_codec;
    entry->config_copy.encoder   = entry->cfg_encoder;
    entry->config_copy.sink_type = entry->cfg_sink_type;
    entry->config_copy.srt_uri   = entry->cfg_srt_uri;
    entry->config_copy.srt_mode  = entry->cfg_srt_mode;
    entry->config_copy.srt_stream_key = entry->cfg_srt_stream_key;
    entry->config_copy.rtmp_uri  = entry->cfg_rtmp_uri;
    entry->config_copy.rtmp_passcode = entry->cfg_rtmp_passcode;
    entry->config_copy.rtmp_plugin = entry->cfg_rtmp_plugin;
    entry->config_copy.file_path = entry->cfg_file_path;
    entry->config_copy.file_path_mode = entry->cfg_file_path_mode;
    entry->config_copy.file_prefix = entry->cfg_file_prefix;
    entry->config_copy.file_container = entry->cfg_file_container;

    /* Insert into hash table (key is borrowed from entry->output_id) */
    g_hash_table_insert(sup->outputs, entry->output_id, entry);

    /* Launch the worker */
    int rc = output_entry_launch(entry);
    if (rc != SBS_OK) {
        LOG_E("failed to launch output worker '%s': %d", config->output_id, rc);
        g_hash_table_remove(sup->outputs, config->output_id);
        return rc;
    }

    LOG_I("output '%s' started (sink=%s, pid=%d)",
          entry->output_id,
          entry->cfg_sink_type ? entry->cfg_sink_type : "srt",
          (int)entry->pid);
    return SBS_OK;
}

/* ── Stop Output ──────────────────────────────────────────────── */

int sbs_output_supervisor_stop_output(sbs_output_supervisor_t *sup,
                                       const char *output_id)
{
    if (!sup || !output_id) return SBS_ERR_INVAL;

    sbs_output_entry_t *entry = g_hash_table_lookup(sup->outputs, output_id);
    if (!entry) return SBS_ERR_NOT_FOUND;

    entry->shutting_down = true;

    /* Cancel any pending restart timer */
    if (entry->restart_timer_id) {
        g_source_remove(entry->restart_timer_id);
        entry->restart_timer_id = 0;
    }

    /* Send shutdown message if connected */
    if (entry->conn_fd >= 0) {
        sbs_shutdown_msg_t msg;
        sbs_shutdown_msg_init(&msg, SHUTDOWN_GRACE_MS, 0);
        sbs_ipc_send_msg(entry->conn_fd, &msg, sizeof(msg));
    }

    /* SIGTERM the worker. The child_watch callback (on_child_exit) will
     * handle final cleanup — closing sockets, removing from hash table,
     * and freeing the entry.  Do NOT remove from hash table here or
     * output_entry_free will block the main loop reaping the child. */
    if (entry->pid > 0) {
        kill(entry->pid, SIGTERM);
    } else {
        /* No pid means the worker was never started or already exited.
         * Remove immediately. */
        g_hash_table_remove(sup->outputs, output_id);
    }
    return SBS_OK;
}

/* ── Send Frame to All Outputs ────────────────────────────────── */

int sbs_output_supervisor_send_frame(sbs_output_supervisor_t *sup,
                                      const sbs_video_frame_msg_t *msg,
                                      int dmabuf_fd)
{
    if (!sup || !msg) return 0;

    int sent_count = 0;
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, sup->outputs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_output_entry_t *entry = value;

        /* Skip entries that aren't connected or are shutting down */
        if (!entry->connected || entry->shutting_down || entry->conn_fd < 0) {
            continue;
        }

        /* dup() the fd — each worker gets its own copy */
        int dup_fd = dup(dmabuf_fd);
        if (dup_fd < 0) {
            LOG_W("output '%s' dup(fd=%d) failed: %s",
                  entry->output_id, dmabuf_fd, strerror(errno));
            entry->frames_dropped++;
            continue;
        }

        int rc = sbs_ipc_send_frame(entry->conn_fd, msg, dup_fd);
        if (rc == SBS_ERR_WOULD_BLOCK) {
            /* Slow consumer — drop this frame for this output */
            close(dup_fd);
            entry->frames_dropped++;
            if (entry->frames_dropped % 100 == 1) {
                LOG_D("output '%s' frame dropped (total dropped: %lu)",
                      entry->output_id, (unsigned long)entry->frames_dropped);
            }
        } else if (rc != SBS_OK) {
            /* Send error — connection may be broken */
            close(dup_fd);
            entry->frames_dropped++;
            LOG_W("output '%s' send error: %d", entry->output_id, rc);
            if (rc == SBS_ERR_PIPE || rc == SBS_ERR_IO) {
                entry->connected = false;
                if (entry->conn_fd >= 0) {
                    close(entry->conn_fd);
                    entry->conn_fd = -1;
                }
                if (!entry->shutting_down && entry->pid > 0) {
                    kill(entry->pid, SIGKILL);
                }
            }
        } else {
            /* Success — fd ownership transferred to kernel for SCM_RIGHTS */
            entry->frames_sent++;
            sent_count++;
        }
    }

    return sent_count;
}

int sbs_output_supervisor_send_audio(sbs_output_supervisor_t *sup,
                                     const sbs_audio_buffer_msg_t *msg,
                                     const void *audio_data)
{
    if (!sup || !msg || !audio_data) return 0;

    int sent_count = 0;
    GHashTableIter iter;
    gpointer value;
    const size_t total = sizeof(*msg) + msg->data_size;
    uint8_t *packet = malloc(total);
    if (!packet) return 0;
    memcpy(packet, msg, sizeof(*msg));
    memcpy(packet + sizeof(*msg), audio_data, msg->data_size);

    g_hash_table_iter_init(&iter, sup->outputs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_output_entry_t *entry = value;
        if (!entry->connected || entry->shutting_down || entry->conn_fd < 0) {
            continue;
        }
        int rc = sbs_ipc_send_msg(entry->conn_fd, packet, total);
        if (rc == SBS_OK) {
            sent_count++;
        }
    }
    free(packet);
    return sent_count;
}

/* ── Shutdown All ─────────────────────────────────────────────── */

void sbs_output_supervisor_shutdown_all(sbs_output_supervisor_t *sup)
{
    if (!sup || !sup->outputs) return;

    uint32_t count = (uint32_t)g_hash_table_size(sup->outputs);
    if (count == 0) return;

    LOG_I("shutting down %u output worker(s)", count);

    /* Collect all entries (can't modify hash table during iteration) */
    GList *entries = g_hash_table_get_values(sup->outputs);

    /* Stage 0: Send SBS_MSG_SHUTDOWN to all workers */
    for (GList *l = entries; l; l = l->next) {
        sbs_output_entry_t *entry = l->data;
        entry->shutting_down = true;

        if (entry->conn_fd >= 0) {
            sbs_shutdown_msg_t msg;
            sbs_shutdown_msg_init(&msg, SHUTDOWN_GRACE_MS, 0);
            sbs_ipc_send_msg(entry->conn_fd, &msg, sizeof(msg));
            LOG_D("sent SHUTDOWN to output '%s' (pid=%d)",
                  entry->output_id, (int)entry->pid);
        }
    }

    /* Stage 1: Wait up to SHUTDOWN_GRACE_MS for clean exit */
    int64_t deadline = g_get_monotonic_time() + (SHUTDOWN_GRACE_MS * 1000);
    bool all_exited = false;

    while (!all_exited && g_get_monotonic_time() < deadline) {
        all_exited = true;
        for (GList *l = entries; l; l = l->next) {
            sbs_output_entry_t *entry = l->data;
            if (entry->pid > 0) {
                int wstatus;
                pid_t result = waitpid(entry->pid, &wstatus, WNOHANG);
                if (result > 0) {
                    LOG_D("output '%s' (pid=%d) exited cleanly",
                          entry->output_id, (int)entry->pid);
                    entry->pid = 0;
                } else {
                    all_exited = false;
                }
            }
        }
        if (!all_exited) {
            g_usleep(50000);  /* 50ms poll interval */
        }
    }

    if (all_exited) {
        LOG_I("all output workers exited cleanly");
        g_list_free(entries);
        g_hash_table_remove_all(sup->outputs);
        return;
    }

    /* Stage 2: SIGTERM remaining workers */
    for (GList *l = entries; l; l = l->next) {
        sbs_output_entry_t *entry = l->data;
        if (entry->pid > 0) {
            LOG_W("sending SIGTERM to output '%s' (pid=%d)",
                  entry->output_id, (int)entry->pid);
            kill(entry->pid, SIGTERM);
        }
    }

    /* Wait up to SHUTDOWN_SIGTERM_MS */
    deadline = g_get_monotonic_time() + (SHUTDOWN_SIGTERM_MS * 1000);
    all_exited = false;

    while (!all_exited && g_get_monotonic_time() < deadline) {
        all_exited = true;
        for (GList *l = entries; l; l = l->next) {
            sbs_output_entry_t *entry = l->data;
            if (entry->pid > 0) {
                int wstatus;
                pid_t result = waitpid(entry->pid, &wstatus, WNOHANG);
                if (result > 0) {
                    entry->pid = 0;
                } else {
                    all_exited = false;
                }
            }
        }
        if (!all_exited) {
            g_usleep(50000);
        }
    }

    /* Stage 3: SIGKILL stragglers */
    for (GList *l = entries; l; l = l->next) {
        sbs_output_entry_t *entry = l->data;
        if (entry->pid > 0) {
            LOG_W("SIGKILL output '%s' (pid=%d)",
                  entry->output_id, (int)entry->pid);
            kill(entry->pid, SIGKILL);
            waitpid(entry->pid, NULL, 0);
            entry->pid = 0;
        }
    }

    g_list_free(entries);
    g_hash_table_remove_all(sup->outputs);
    LOG_I("all output workers stopped");
}

/* ── Output Entry Free ────────────────────────────────────────── */

static void output_entry_free(sbs_output_entry_t *entry)
{
    if (!entry) return;

    /* Kill worker if still running and reap synchronously. */
    if (entry->pid > 0) {
        kill(entry->pid, SIGKILL);
        for (int i = 0; i < 100; i++) {
            if (waitpid(entry->pid, NULL, WNOHANG) > 0) break;
            if (errno == ECHILD) break;
            usleep(5000);
        }
        entry->pid = 0;
    }

    /* Remove GLib watches — pid is 0 so child watch won't block. */
    if (entry->io_watch_id) {
        guint wid = entry->io_watch_id;
        entry->io_watch_id = 0;
        g_source_remove(wid);
    }
    if (entry->child_watch_id) {
        guint wid = entry->child_watch_id;
        entry->child_watch_id = 0;
        g_source_remove(wid);
    }
    if (entry->accept_timer_id) {
        guint wid = entry->accept_timer_id;
        entry->accept_timer_id = 0;
        g_source_remove(wid);
    }
    if (entry->stable_timer_id) {
        guint wid = entry->stable_timer_id;
        entry->stable_timer_id = 0;
        g_source_remove(wid);
    }
    if (entry->restart_timer_id) {
        guint wid = entry->restart_timer_id;
        entry->restart_timer_id = 0;
        g_source_remove(wid);
    }

    /* Close sockets */
    if (entry->conn_fd >= 0)   close(entry->conn_fd);
    if (entry->listen_fd >= 0) close(entry->listen_fd);

    /* Unlink socket file */
    if (entry->socket_path) {
        unlink(entry->socket_path);
    }

    /* Free deep-copied config strings */
    free(entry->cfg_codec);
    free(entry->cfg_encoder);
    free(entry->cfg_sink_type);
    free(entry->cfg_srt_uri);
    free(entry->cfg_srt_mode);
    free(entry->cfg_srt_stream_key);
    free(entry->cfg_rtmp_uri);
    free(entry->cfg_rtmp_passcode);
    free(entry->cfg_rtmp_plugin);
    free(entry->cfg_file_path);
    free(entry->cfg_file_path_mode);
    free(entry->cfg_file_prefix);
    free(entry->cfg_file_container);

    free(entry->output_id);
    free(entry->socket_path);
    free(entry);
}

/* ── Worker Launch ────────────────────────────────────────────── */

/**
 * Build JSON config string for the output worker.
 * Returns heap-allocated string; caller must free.
 */
static char *build_config_json(const sbs_output_entry_t *entry)
{
    const sbs_output_start_config_t *cfg = &entry->config_copy;
    cJSON *obj = cJSON_CreateObject();
    char *json;

    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "mode", "output");
    cJSON_AddStringToObject(obj, "worker_id", entry->output_id ? entry->output_id : "");
    cJSON_AddStringToObject(obj, "socket_path", entry->socket_path ? entry->socket_path : "");
    cJSON_AddNumberToObject(obj, "width", cfg->width);
    cJSON_AddNumberToObject(obj, "height", cfg->height);
    cJSON_AddNumberToObject(obj, "framerate_num", cfg->framerate_num);
    cJSON_AddNumberToObject(obj, "framerate_den", cfg->framerate_den > 0 ? cfg->framerate_den : 1);

    if (cfg->codec) {
        cJSON_AddStringToObject(obj, "codec", cfg->codec);
    }
    if (cfg->bitrate_kbps > 0) {
        cJSON_AddNumberToObject(obj, "bitrate_kbps", cfg->bitrate_kbps);
    }
    if (cfg->encoder) {
        cJSON_AddStringToObject(obj, "encoder", cfg->encoder);
    }
    if (cfg->sink_type) {
        cJSON_AddStringToObject(obj, "sink_type", cfg->sink_type);
    }
    if (cfg->srt_uri) {
        cJSON_AddStringToObject(obj, "srt_uri", cfg->srt_uri);
    }
    if (cfg->srt_mode) {
        cJSON_AddStringToObject(obj, "srt_mode", cfg->srt_mode);
    }
    if (cfg->srt_stream_key) {
        cJSON_AddStringToObject(obj, "srt_stream_key", cfg->srt_stream_key);
    }
    if (cfg->srt_latency_ms > 0) {
        cJSON_AddNumberToObject(obj, "srt_latency_ms", cfg->srt_latency_ms);
    }
    if (cfg->rtmp_uri) {
        cJSON_AddStringToObject(obj, "rtmp_uri", cfg->rtmp_uri);
    }
    if (cfg->rtmp_passcode) {
        cJSON_AddStringToObject(obj, "rtmp_passcode", cfg->rtmp_passcode);
    }
    if (cfg->rtmp_plugin) {
        cJSON_AddStringToObject(obj, "rtmp_plugin", cfg->rtmp_plugin);
    }
    if (cfg->file_path) {
        cJSON_AddStringToObject(obj, "file_path", cfg->file_path);
    }
    if (cfg->file_path_mode) {
        cJSON_AddStringToObject(obj, "file_path_mode", cfg->file_path_mode);
    }
    if (cfg->file_prefix) {
        cJSON_AddStringToObject(obj, "file_prefix", cfg->file_prefix);
    }
    if (cfg->file_container) {
        cJSON_AddStringToObject(obj, "file_container", cfg->file_container);
    }
    if (cfg->gop_size > 0) {
        cJSON_AddNumberToObject(obj, "gop_size", cfg->gop_size);
    }

    json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/**
 * Create, bind, and listen on a Unix domain socket.
 * Returns the listening fd on success, -1 on error.
 */
static int create_listen_socket(const char *socket_path)
{
    /* Remove stale socket file */
    unlink(socket_path);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_E("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("bind(%s) failed: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Allow only the worker (same user) to connect */
    chmod(socket_path, 0600);

    if (listen(fd, 1) < 0) {
        LOG_E("listen(%s) failed: %s", socket_path, strerror(errno));
        close(fd);
        unlink(socket_path);
        return -1;
    }

    return fd;
}

/**
 * Accept a single connection on the listening socket (non-blocking check).
 * Sets socket buffer sizes and O_NONBLOCK on the accepted fd.
 * Returns the accepted fd, or -1 if not ready.
 */
static int try_accept_connection(int listen_fd)
{
    int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (conn_fd < 0) {
        return -1;
    }

    /* Set socket buffer sizes */
    int rcvbuf = SOCKET_RCVBUF_SIZE;
    int sndbuf = SOCKET_SNDBUF_SIZE;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(conn_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    return conn_fd;
}

/**
 * Fork/exec the output worker process.
 */
static int output_entry_launch(sbs_output_entry_t *entry)
{
    sbs_output_supervisor_t *sup = entry->supervisor;

    /* Ensure socket directory exists */
    mkdir(sup->sock_dir, 0755);

    /* Create listening socket */
    entry->listen_fd = create_listen_socket(entry->socket_path);
    if (entry->listen_fd < 0) {
        return SBS_ERR_IO;
    }

    /* Build JSON config for stdin */
    char *config_json = build_config_json(entry);
    if (!config_json) {
        close(entry->listen_fd);
        entry->listen_fd = -1;
        return SBS_ERR_NOMEM;
    }

    /* Create a pipe for stdin */
    int stdin_pipe[2];
    if (pipe2(stdin_pipe, O_CLOEXEC) < 0) {
        LOG_E("pipe2 failed: %s", strerror(errno));
        free(config_json);
        close(entry->listen_fd);
        entry->listen_fd = -1;
        return SBS_ERR_IO;
    }

    /* Fork */
    pid_t pid = fork();
    if (pid < 0) {
        LOG_E("fork failed: %s", strerror(errno));
        free(config_json);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(entry->listen_fd);
        entry->listen_fd = -1;
        return SBS_ERR_IO;
    }

    if (pid == 0) {
        /* ── Child process ─────────────────────────────────── */

        /* Redirect stdin to the read end of the pipe */
        close(stdin_pipe[1]);
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(stdin_pipe[0]);

        /* Close the listening socket fd in the child —
         * the worker will connect as a client */
        close(entry->listen_fd);

        /* Exec the worker */
        execl(sup->worker_path, "sbs-worker",
              "--mode", "output",
              "--id", entry->output_id,
              "--socket", entry->socket_path,
              (char *)NULL);

        /* exec failed */
        _exit(127);
    }

    /* ── Parent process ────────────────────────────────────── */
    close(stdin_pipe[0]);  /* Close read end */

    /* Write config JSON to worker's stdin, then close */
    size_t json_len = strlen(config_json);
    ssize_t written = write(stdin_pipe[1], config_json, json_len);
    close(stdin_pipe[1]);  /* Close write end → worker sees EOF on stdin */
    free(config_json);

    if (written < 0 || (size_t)written != json_len) {
        LOG_W("partial config write to output worker stdin (%zd/%zu)",
              written, json_len);
    }

    entry->pid = pid;
    entry->last_start_time = g_get_monotonic_time();

    /* Install SIGCHLD watch */
    entry->child_watch_id = g_child_watch_add(pid, on_child_exit, entry);

    /* Set up accept timeout — worker should connect within ACCEPT_TIMEOUT_MS */
    entry->accept_timer_id = g_timeout_add(ACCEPT_TIMEOUT_MS, on_accept_timeout, entry);

    /* Make listening socket non-blocking for accept polling */
    int flags = fcntl(entry->listen_fd, F_GETFL, 0);
    fcntl(entry->listen_fd, F_SETFL, flags | O_NONBLOCK);

    /* Try to accept immediately (worker might connect fast) */
    int conn_fd = try_accept_connection(entry->listen_fd);
    if (conn_fd >= 0) {
        entry->conn_fd = conn_fd;
        entry->connected = true;
        if (entry->accept_timer_id) {
            g_source_remove(entry->accept_timer_id);
            entry->accept_timer_id = 0;
        }

        /* Install I/O watch for STATUS messages from worker */
        entry->io_watch_id = g_unix_fd_add(entry->conn_fd,
                                            G_IO_IN | G_IO_HUP | G_IO_ERR,
                                            on_worker_data, entry);

        /* Start stable timer */
        entry->stable_timer_id = g_timeout_add(STABLE_RESET_MS,
                                                on_stable_timer, entry);

        LOG_D("output '%s' connected immediately", entry->output_id);
    } else {
        /* Install I/O watch on listening socket for deferred accept */
        entry->io_watch_id = g_unix_fd_add(entry->listen_fd,
                                            G_IO_IN,
                                            on_worker_data, entry);
    }

    return SBS_OK;
}

/**
 * Clean up worker process state without freeing the entry.
 * Used before restart.
 */
static void output_entry_cleanup_worker(sbs_output_entry_t *entry)
{
    if (entry->io_watch_id) {
        g_source_remove(entry->io_watch_id);
        entry->io_watch_id = 0;
    }
    if (entry->child_watch_id) {
        g_source_remove(entry->child_watch_id);
        entry->child_watch_id = 0;
    }
    if (entry->accept_timer_id) {
        g_source_remove(entry->accept_timer_id);
        entry->accept_timer_id = 0;
    }
    if (entry->stable_timer_id) {
        g_source_remove(entry->stable_timer_id);
        entry->stable_timer_id = 0;
    }
    if (entry->restart_timer_id) {
        g_source_remove(entry->restart_timer_id);
        entry->restart_timer_id = 0;
    }

    if (entry->conn_fd >= 0) {
        close(entry->conn_fd);
        entry->conn_fd = -1;
    }
    if (entry->listen_fd >= 0) {
        close(entry->listen_fd);
        entry->listen_fd = -1;
    }
    if (entry->socket_path) {
        unlink(entry->socket_path);
    }

    entry->pid = 0;
    entry->connected = false;
}

/* ── Worker Data Reception (STATUS messages) ──────────────────── */

/**
 * Handle incoming data from the output worker.
 * Output workers send STATUS heartbeats back to the supervisor.
 * Also handles deferred accept on the listening socket.
 */
static gboolean on_worker_data(gint fd, GIOCondition cond, gpointer user_data)
{
    sbs_output_entry_t *entry = user_data;

    /* If this is the listening socket waiting for accept */
    if (!entry->connected && fd == entry->listen_fd) {
        int conn_fd = try_accept_connection(entry->listen_fd);
        if (conn_fd < 0) {
            return G_SOURCE_CONTINUE;
        }

        entry->conn_fd = conn_fd;
        entry->connected = true;

        /* Cancel accept timeout */
        if (entry->accept_timer_id) {
            g_source_remove(entry->accept_timer_id);
            entry->accept_timer_id = 0;
        }

        /* Re-install I/O watch on the connected socket instead */
        entry->io_watch_id = g_unix_fd_add(entry->conn_fd,
                                            G_IO_IN | G_IO_HUP | G_IO_ERR,
                                            on_worker_data, entry);

        /* Start stable timer */
        entry->stable_timer_id = g_timeout_add(STABLE_RESET_MS,
                                                on_stable_timer, entry);

        LOG_I("output '%s' worker connected", entry->output_id);

        /* Return REMOVE for the old watch (on listen_fd) */
        return G_SOURCE_REMOVE;
    }

    /* Handle error/hangup conditions */
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        LOG_W("output '%s' connection lost (HUP/ERR)", entry->output_id);
        close(entry->conn_fd);
        entry->conn_fd = -1;
        entry->connected = false;
        entry->io_watch_id = 0;
        if (!entry->shutting_down && entry->pid > 0) {
            kill(entry->pid, SIGKILL);
        }
        /* Child watch callback will handle restart */
        return G_SOURCE_REMOVE;
    }

    /* Read control message from connected socket (STATUS, ERROR, etc.).
     * Output workers don't send frame data back — only control messages. */
    union {
        sbs_status_msg_t  status;
        uint8_t           raw[512];
    } msg_buf;
    memset(&msg_buf, 0, sizeof(msg_buf));

    size_t bytes_read = 0;
    int rc = sbs_ipc_recv_msg(entry->conn_fd, &msg_buf, sizeof(msg_buf), &bytes_read);

    if (rc == SBS_ERR_WOULD_BLOCK) {
        return G_SOURCE_CONTINUE;
    }

    if (rc == SBS_ERR_EOF) {
        LOG_D("output '%s' connection closed (EOF)", entry->output_id);
        close(entry->conn_fd);
        entry->conn_fd = -1;
        entry->connected = false;
        entry->io_watch_id = 0;
        if (!entry->shutting_down && entry->pid > 0) {
            kill(entry->pid, SIGKILL);
        }
        return G_SOURCE_REMOVE;
    }

    if (rc != SBS_OK) {
        LOG_W("output '%s' recv error: %d", entry->output_id, rc);
        return G_SOURCE_CONTINUE;
    }

    /* Dispatch based on message type */
    uint32_t msg_type = msg_buf.status.header.msg_type;

    if (msg_type == SBS_IPC_MSG_STATUS) {
        LOG_T("output '%s' status: state=%u consumed=%lu encoded=%lu",
              entry->output_id, msg_buf.status.state,
              (unsigned long)msg_buf.status.frames_consumed,
              (unsigned long)msg_buf.status.frames_encoded);
    } else {
        LOG_D("output '%s' unknown msg type 0x%04x", entry->output_id, msg_type);
    }

    return G_SOURCE_CONTINUE;
}

/* ── Child Exit Handler ───────────────────────────────────────── */

static void on_child_exit(GPid pid, gint status, gpointer user_data)
{
    sbs_output_entry_t *entry = user_data;
    entry->child_watch_id = 0;

    g_spawn_close_pid(pid);

    if (WIFEXITED(status)) {
        LOG_I("output '%s' (pid=%d) exited with code %d",
              entry->output_id, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LOG_W("output '%s' (pid=%d) killed by signal %d",
              entry->output_id, (int)pid, WTERMSIG(status));
    }

    entry->pid = 0;
    entry->connected = false;

    /* Clean up socket state */
    if (entry->io_watch_id) {
        g_source_remove(entry->io_watch_id);
        entry->io_watch_id = 0;
    }
    if (entry->conn_fd >= 0) {
        close(entry->conn_fd);
        entry->conn_fd = -1;
    }
    if (entry->listen_fd >= 0) {
        close(entry->listen_fd);
        entry->listen_fd = -1;
    }
    if (entry->socket_path) {
        unlink(entry->socket_path);
    }

    /* Don't restart if shutting down */
    if (entry->shutting_down) {
        LOG_D("output '%s' not restarting (shutting down)", entry->output_id);
        /* Remove from hash table only if we are still the current entry.
         * A new entry may have been inserted for the same output_id if
         * output.update restarted the output before this callback fired. */
        if (entry->supervisor && entry->supervisor->outputs) {
            sbs_output_entry_t *current = g_hash_table_lookup(entry->supervisor->outputs, entry->output_id);
            if (current == entry) {
                g_hash_table_remove(entry->supervisor->outputs, entry->output_id);
            }
        }
        return;
    }

    /* Schedule restart with backoff */
    schedule_restart(entry);
}

/* ── Accept Timeout ───────────────────────────────────────────── */

static gboolean on_accept_timeout(gpointer user_data)
{
    sbs_output_entry_t *entry = user_data;
    entry->accept_timer_id = 0;

    LOG_W("output '%s' worker did not connect within %d ms",
          entry->output_id, ACCEPT_TIMEOUT_MS);

    /* Kill the worker and trigger restart */
    if (entry->pid > 0) {
        kill(entry->pid, SIGKILL);
    }

    return G_SOURCE_REMOVE;
}

/* ── Stable Timer ─────────────────────────────────────────────── */

/**
 * After STABLE_RESET_MS of successful operation, reset the restart counter.
 */
static gboolean on_stable_timer(gpointer user_data)
{
    sbs_output_entry_t *entry = user_data;
    entry->stable_timer_id = 0;

    if (entry->restart_attempts > 0) {
        LOG_I("output '%s' stable for %d s, resetting restart counter (was %u)",
              entry->output_id, STABLE_RESET_MS / 1000,
              entry->restart_attempts);
        entry->restart_attempts = 0;
    }

    return G_SOURCE_REMOVE;
}

/* ── Exponential Backoff Restart ──────────────────────────────── */

static void schedule_restart(sbs_output_entry_t *entry)
{
    entry->restart_attempts++;

    if (entry->restart_attempts > MAX_RESTART_ATTEMPTS) {
        LOG_E("output '%s' exceeded max restart attempts (%d), giving up",
              entry->output_id, MAX_RESTART_ATTEMPTS);
        return;
    }

    /* Calculate backoff: min(base * 2^attempt, max) + jitter */
    uint32_t delay_ms = BACKOFF_BASE_MS;
    for (uint32_t i = 1; i < entry->restart_attempts && delay_ms < BACKOFF_MAX_MS; i++) {
        delay_ms *= 2;
    }
    if (delay_ms > BACKOFF_MAX_MS) {
        delay_ms = BACKOFF_MAX_MS;
    }

    /* Add jitter: +/- 25% */
    uint32_t jitter = (uint32_t)(g_random_int_range(0, (gint32)(delay_ms / 4)));
    if (g_random_boolean()) {
        delay_ms += jitter;
    } else if (delay_ms > jitter) {
        delay_ms -= jitter;
    }

    LOG_I("output '%s' scheduling restart in %u ms (attempt %u/%d)",
          entry->output_id, delay_ms, entry->restart_attempts,
          MAX_RESTART_ATTEMPTS);

    entry->restart_timer_id = g_timeout_add(delay_ms, on_restart_timer, entry);
}

static gboolean on_restart_timer(gpointer user_data)
{
    sbs_output_entry_t *entry = user_data;
    entry->restart_timer_id = 0;

    if (entry->shutting_down) {
        LOG_D("output '%s' skipping restart (shutting down)", entry->output_id);
        return G_SOURCE_REMOVE;
    }

    LOG_I("restarting output '%s' (attempt %u)", entry->output_id,
          entry->restart_attempts);

    /* Clean up previous worker state */
    output_entry_cleanup_worker(entry);

    /* Relaunch */
    int rc = output_entry_launch(entry);
    if (rc != SBS_OK) {
        LOG_E("output '%s' relaunch failed: %d", entry->output_id, rc);
        /* Schedule another attempt */
        schedule_restart(entry);
    }

    return G_SOURCE_REMOVE;
}
