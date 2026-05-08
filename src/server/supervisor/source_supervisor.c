/*
 * SBS - StreamBox Broadcast System
 * Source Supervisor — fork/exec source workers, receive frames, crash recovery
 *
 * The supervisor runs on the main thread (GLib main loop) and:
 *   1. Creates a Unix socket for each source worker
 *   2. Fork/execs sbs-worker --mode=source, pipes JSON config to stdin
 *   3. Accepts the worker connection and installs a GLib I/O watch
 *   4. Receives video frames via recvmsg (SCM_RIGHTS for DMA-BUF fds)
 *   5. Updates the compositor's frame slot (lock-free atomic swap)
 *   6. Monitors worker health via SIGCHLD and heartbeat timeouts
 *   7. Restarts crashed workers with exponential backoff
 *   8. Implements 4-stage graceful shutdown
 *
 * Reference: document/05-source-manager.md sections 3-7
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "src-sup"

#include "sbs/source_supervisor.h"
#include "sbs/api_server.h"
#include "sbs/scene_graph.h"
#include "sbs/ipc.h"
#include "sbs/ipc_transport.h"
#include "sbs/log.h"

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

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif

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
#define SOURCE_STALE_USEC       (5 * G_USEC_PER_SEC)

/* ── Source Entry (per-worker state) ──────────────────────────── */

typedef struct sbs_source_entry {
    /* Identity */
    char        *source_id;
    char        *source_type;
    char        *socket_path;

    /* Worker process */
    GPid         pid;
    int          listen_fd;       /* Listening socket fd */
    int          conn_fd;         /* Accepted connection fd (-1 if not connected) */

    /* Frame slot (owned by caller, borrowed reference) */
    sbs_frame_slot_t *frame_slot;

    /* GLib watch IDs (0 = not installed) */
    guint        io_watch_id;     /* I/O watch on conn_fd */
    guint        child_watch_id;  /* SIGCHLD watch on pid */
    guint        accept_timer_id; /* Timer for accept timeout */
    guint        stable_timer_id; /* Timer to reset backoff after stable operation */

    /* Health & recovery */
    uint32_t     restart_attempts;  /* Consecutive failures since last stable period */
    int64_t      last_start_time;   /* g_get_monotonic_time() when last started */
    int64_t      last_frame_time;   /* Monotonic time of last received frame */
    bool         shutting_down;     /* Set when stop requested */
    bool         muted;             /* Drop frames without publishing to slot */

    /* Configuration (stored for restart) */
    sbs_source_start_config_t config_copy;

    /* Back-reference to supervisor */
    struct sbs_source_supervisor *supervisor;
} sbs_source_entry_t;

/* ── Supervisor ───────────────────────────────────────────────── */

struct sbs_source_supervisor {
    GHashTable           *sources;      /* source_id → sbs_source_entry_t* */
    char                 *worker_path;  /* Path to sbs-worker binary */
    char                 *sock_dir;     /* Directory for IPC sockets */
    sbs_scene_graph_t    *scene_graph;  /* For updating frame dimensions */
    sbs_api_server_t     *api_server;   /* For scene refresh on dimension change */
};

/* ── Forward Declarations ─────────────────────────────────────── */

static void     source_entry_free(sbs_source_entry_t *entry);
static int      source_entry_launch(sbs_source_entry_t *entry);
static void     source_entry_cleanup_worker(sbs_source_entry_t *entry);
static char    *build_config_json(const sbs_source_entry_t *entry);
static gboolean on_frame_available(gint fd, GIOCondition cond, gpointer user_data);
static void     on_child_exit(GPid pid, gint status, gpointer user_data);
static gboolean on_accept_timeout(gpointer user_data);
static gboolean on_stable_timer(gpointer user_data);
static void     schedule_restart(sbs_source_entry_t *entry);
static gboolean on_restart_timer(gpointer user_data);

static void send_source_frame_release(int sock_fd, const sbs_video_frame_msg_t *frame)
{
    if (sock_fd < 0 || !frame || !(frame->flags & SBS_FRAME_FLAG_NEEDS_RELEASE))
        return;

    sbs_frame_release_msg_t msg;
    sbs_frame_release_msg_init(&msg);
    msg.sequence = frame->sequence;
    (void)sbs_ipc_send_msg(sock_fd, &msg, sizeof(msg));
}

static bool source_type_is_static(const char *source_type)
{
    return source_type && strcmp(source_type, "image") == 0;
}

/* ── Supervisor Lifecycle ─────────────────────────────────────── */

sbs_source_supervisor_t *sbs_source_supervisor_new(const char *worker_path,
                                                    const char *sock_dir)
{
    sbs_source_supervisor_t *sup = calloc(1, sizeof(*sup));
    if (!sup) return NULL;

    sup->sources     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, /* key is owned by entry */
                                              (GDestroyNotify)source_entry_free);
    sup->worker_path = strdup(worker_path);
    sup->sock_dir    = strdup(sock_dir);

    if (!sup->sources || !sup->worker_path || !sup->sock_dir) {
        sbs_source_supervisor_free(sup);
        return NULL;
    }

    LOG_I("source supervisor created (worker=%s, sock_dir=%s)",
          worker_path, sock_dir);
    return sup;
}

void sbs_source_supervisor_free(sbs_source_supervisor_t *sup)
{
    if (!sup) return;

    if (sup->sources) {
        /* This will call source_entry_free for each entry */
        g_hash_table_destroy(sup->sources);
    }
    free(sup->worker_path);
    free(sup->sock_dir);
    free(sup);
}

uint32_t sbs_source_supervisor_source_count(const sbs_source_supervisor_t *sup)
{
    if (!sup || !sup->sources) return 0;
    return (uint32_t)g_hash_table_size(sup->sources);
}

void sbs_source_supervisor_get_metrics(const sbs_source_supervisor_t *sup,
                                       sbs_source_supervisor_metrics_t *metrics)
{
    GHashTableIter iter;
    gpointer value;
    int64_t now;

    if (!metrics) {
        return;
    }

    memset(metrics, 0, sizeof(*metrics));
    if (!sup || !sup->sources) {
        return;
    }

    now = g_get_monotonic_time();
    g_hash_table_iter_init(&iter, sup->sources);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const sbs_source_entry_t *entry = value;
        bool connected = entry->pid > 0 && entry->conn_fd >= 0;
        bool restarting = !entry->shutting_down && entry->restart_attempts > 0 && !connected;
        bool stale = connected && !source_type_is_static(entry->source_type) &&
                     entry->last_frame_time > 0 &&
                     (now - entry->last_frame_time) > SOURCE_STALE_USEC;

        metrics->total_sources++;
        if (connected) {
            metrics->connected_sources++;
        }
        if (restarting) {
            metrics->restarting_sources++;
        }
        if (stale) {
            metrics->stale_sources++;
        }
        if (entry->restart_attempts > metrics->max_restart_attempts) {
            metrics->max_restart_attempts = entry->restart_attempts;
        }
    }

    metrics->degraded = metrics->restarting_sources > 0 || metrics->stale_sources > 0;
}

void sbs_source_supervisor_set_scene_graph(sbs_source_supervisor_t *sup,
                                            sbs_scene_graph_t *graph)
{
    if (sup) sup->scene_graph = graph;
}

void sbs_source_supervisor_set_api_server(sbs_source_supervisor_t *sup,
                                          sbs_api_server_t *server)
{
    if (sup) sup->api_server = server;
}

/* ── Start Source ─────────────────────────────────────────────── */

int sbs_source_supervisor_start_source(sbs_source_supervisor_t *sup,
                                        const sbs_source_start_config_t *config,
                                        sbs_frame_slot_t *slot)
{
    if (!sup || !config || !config->source_id || !slot) {
        return SBS_ERR_INVAL;
    }

    /* Check for duplicate */
    if (g_hash_table_contains(sup->sources, config->source_id)) {
        LOG_W("source '%s' already exists", config->source_id);
        return SBS_ERR_INVAL;
    }

    /* Create entry */
    sbs_source_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return SBS_ERR_NOMEM;

    entry->source_id   = strdup(config->source_id);
    entry->source_type = strdup(config->source_type ? config->source_type : "videotestsrc");
    entry->listen_fd   = -1;
    entry->conn_fd     = -1;
    entry->pid         = 0;
    entry->frame_slot  = slot;
    entry->supervisor  = sup;

    /* Build socket path */
    size_t path_len = strlen(sup->sock_dir) + strlen(config->source_id) + 16;
    entry->socket_path = malloc(path_len);
    if (!entry->socket_path) {
        source_entry_free(entry);
        return SBS_ERR_NOMEM;
    }
    snprintf(entry->socket_path, path_len, "%s/source-%s.sock",
             sup->sock_dir, config->source_id);

    /* Copy config for restarts */
    entry->config_copy = *config;
    /* Deep-copy string fields */
    entry->config_copy.source_id    = entry->source_id;  /* already duped */
    entry->config_copy.source_type  = entry->source_type;
    entry->config_copy.pattern      = config->pattern ? strdup(config->pattern) : NULL;
    entry->config_copy.capture_mode = config->capture_mode ? strdup(config->capture_mode) : NULL;
    entry->config_copy.output_format = config->output_format ? strdup(config->output_format) : NULL;
    entry->config_copy.device_path  = config->device_path ? strdup(config->device_path) : NULL;
    entry->config_copy.uri          = config->uri ? strdup(config->uri) : NULL;
    entry->config_copy.text         = config->text ? strdup(config->text) : NULL;
    entry->config_copy.font_family  = config->font_family ? strdup(config->font_family) : NULL;
    entry->config_copy.font_path    = config->font_path ? strdup(config->font_path) : NULL;
    entry->config_copy.text_color   = config->text_color ? strdup(config->text_color) : NULL;
    entry->config_copy.text_align   = config->text_align ? strdup(config->text_align) : NULL;

    /* Insert into hash table (key is borrowed from entry->source_id) */
    g_hash_table_insert(sup->sources, entry->source_id, entry);

    /* Launch the worker */
    int rc = source_entry_launch(entry);
    if (rc != SBS_OK) {
        LOG_E("failed to launch source worker '%s': %d", config->source_id, rc);
        g_hash_table_remove(sup->sources, config->source_id);
        return rc;
    }

    LOG_I("source '%s' started (type=%s, pid=%d)",
          entry->source_id, entry->source_type, (int)entry->pid);
    return SBS_OK;
}

/* ── Stop Source ──────────────────────────────────────────────── */

int sbs_source_supervisor_stop_source(sbs_source_supervisor_t *sup,
                                       const char *source_id)
{
    if (!sup || !source_id) return SBS_ERR_INVAL;

    sbs_source_entry_t *entry = g_hash_table_lookup(sup->sources, source_id);
    if (!entry) return SBS_ERR_NOT_FOUND;

    entry->shutting_down = true;

    /* Send shutdown message if connected */
    if (entry->conn_fd >= 0) {
        sbs_shutdown_msg_t msg;
        sbs_shutdown_msg_init(&msg, SHUTDOWN_GRACE_MS, 0);
        sbs_ipc_send_msg(entry->conn_fd, &msg, sizeof(msg));
    }

    /* SIGTERM the worker */
    if (entry->pid > 0) {
        kill(entry->pid, SIGTERM);
    }

    /* The child_watch callback will handle final cleanup.
     * Remove from hash table — this triggers source_entry_free */
    g_hash_table_remove(sup->sources, source_id);
    return SBS_OK;
}

int sbs_source_supervisor_mute_source(sbs_source_supervisor_t *sup, const char *source_id)
{
    if (!sup || !source_id) return SBS_ERR_INVAL;
    sbs_source_entry_t *entry = g_hash_table_lookup(sup->sources, source_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    if (entry->muted) return SBS_OK;
    entry->muted = true;
    LOG_I("source '%s' muted (dropping frames)", source_id);
    return SBS_OK;
}

int sbs_source_supervisor_unmute_source(sbs_source_supervisor_t *sup, const char *source_id)
{
    if (!sup || !source_id) return SBS_ERR_INVAL;
    sbs_source_entry_t *entry = g_hash_table_lookup(sup->sources, source_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    if (!entry->muted) return SBS_OK;
    entry->muted = false;
    LOG_I("source '%s' unmuted (publishing frames)", source_id);
    return SBS_OK;
}

/* ── Shutdown All ─────────────────────────────────────────────── */

void sbs_source_supervisor_shutdown_all(sbs_source_supervisor_t *sup)
{
    if (!sup || !sup->sources) return;

    uint32_t count = (uint32_t)g_hash_table_size(sup->sources);
    if (count == 0) return;

    LOG_I("shutting down %u source worker(s)", count);

    /* Collect all entries (can't modify hash table during iteration) */
    GList *entries = g_hash_table_get_values(sup->sources);

    /* Stage 0: Send SBS_MSG_SHUTDOWN to all workers */
    for (GList *l = entries; l; l = l->next) {
        sbs_source_entry_t *entry = l->data;
        entry->shutting_down = true;

        if (entry->conn_fd >= 0) {
            sbs_shutdown_msg_t msg;
            sbs_shutdown_msg_init(&msg, SHUTDOWN_GRACE_MS, 0);
            sbs_ipc_send_msg(entry->conn_fd, &msg, sizeof(msg));
            LOG_D("sent SHUTDOWN to source '%s' (pid=%d)",
                  entry->source_id, (int)entry->pid);
        }
    }

    /* Stage 1: Wait up to SHUTDOWN_GRACE_MS for clean exit */
    int64_t deadline = g_get_monotonic_time() + (SHUTDOWN_GRACE_MS * 1000);
    bool all_exited = false;

    while (!all_exited && g_get_monotonic_time() < deadline) {
        all_exited = true;
        for (GList *l = entries; l; l = l->next) {
            sbs_source_entry_t *entry = l->data;
            if (entry->pid > 0) {
                int wstatus;
                pid_t result = waitpid(entry->pid, &wstatus, WNOHANG);
                if (result > 0) {
                    LOG_D("source '%s' (pid=%d) exited cleanly",
                          entry->source_id, (int)entry->pid);
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
        LOG_I("all source workers exited cleanly");
        g_list_free(entries);
        g_hash_table_remove_all(sup->sources);
        return;
    }

    /* Stage 2: SIGTERM remaining workers */
    for (GList *l = entries; l; l = l->next) {
        sbs_source_entry_t *entry = l->data;
        if (entry->pid > 0) {
            LOG_W("sending SIGTERM to source '%s' (pid=%d)",
                  entry->source_id, (int)entry->pid);
            kill(entry->pid, SIGTERM);
        }
    }

    /* Wait up to SHUTDOWN_SIGTERM_MS */
    deadline = g_get_monotonic_time() + (SHUTDOWN_SIGTERM_MS * 1000);
    all_exited = false;

    while (!all_exited && g_get_monotonic_time() < deadline) {
        all_exited = true;
        for (GList *l = entries; l; l = l->next) {
            sbs_source_entry_t *entry = l->data;
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
        sbs_source_entry_t *entry = l->data;
        if (entry->pid > 0) {
            LOG_W("SIGKILL source '%s' (pid=%d)",
                  entry->source_id, (int)entry->pid);
            kill(entry->pid, SIGKILL);
            waitpid(entry->pid, NULL, 0);
            entry->pid = 0;
        }
    }

    g_list_free(entries);
    g_hash_table_remove_all(sup->sources);
    LOG_I("all source workers stopped");
}

/* ── Source Entry Free ────────────────────────────────────────── */

static void source_entry_free(sbs_source_entry_t *entry)
{
    if (!entry) return;

    /* Remove GLib watches */
    if (entry->io_watch_id)     g_source_remove(entry->io_watch_id);
    if (entry->child_watch_id)  g_source_remove(entry->child_watch_id);
    if (entry->accept_timer_id) g_source_remove(entry->accept_timer_id);
    if (entry->stable_timer_id) g_source_remove(entry->stable_timer_id);

    /* Close sockets */
    if (entry->conn_fd >= 0)   close(entry->conn_fd);
    if (entry->listen_fd >= 0) close(entry->listen_fd);

    /* Unlink socket file */
    if (entry->socket_path) {
        unlink(entry->socket_path);
    }

    /* Kill worker if still running */
    if (entry->pid > 0) {
        kill(entry->pid, SIGKILL);
        waitpid(entry->pid, NULL, 0);
    }

    /* Free config copy strings (those that were strdup'd) */
    free((char *)entry->config_copy.pattern);
    free((char *)entry->config_copy.capture_mode);
    free((char *)entry->config_copy.output_format);
    free((char *)entry->config_copy.device_path);
    free((char *)entry->config_copy.uri);
    free((char *)entry->config_copy.text);
    free((char *)entry->config_copy.font_family);
    free((char *)entry->config_copy.font_path);
    free((char *)entry->config_copy.text_color);
    free((char *)entry->config_copy.text_align);

    free(entry->source_id);
    free(entry->source_type);
    free(entry->socket_path);
    free(entry);
}

/* ── Worker Launch ────────────────────────────────────────────── */

/**
 * Build JSON config string for the worker.
 * Returns heap-allocated string; caller must free.
 */
static char *build_config_json(const sbs_source_entry_t *entry)
{
    GString *json = g_string_new("{");
    char *escaped = NULL;

#define ADD_JSON_STRING(key, value) do { \
        if ((value)) { \
            escaped = g_strescape((value), NULL); \
            g_string_append_printf(json, "%s\"%s\":\"%s\"", \
                                   json->len > 1 ? "," : "", (key), escaped ? escaped : ""); \
            g_free(escaped); \
        } \
    } while (0)

    ADD_JSON_STRING("mode", "source");
    ADD_JSON_STRING("worker_id", entry->source_id);
    ADD_JSON_STRING("socket_path", entry->socket_path);
    g_string_append_printf(json,
                           ",\"width\":%u,\"height\":%u,\"framerate_num\":%u,\"framerate_den\":%u",
                           entry->config_copy.width,
                           entry->config_copy.height,
                           entry->config_copy.framerate_num,
                           entry->config_copy.framerate_den > 0 ? entry->config_copy.framerate_den : 1);
    ADD_JSON_STRING("source_type", entry->source_type);
    ADD_JSON_STRING("pattern", entry->config_copy.pattern);
    ADD_JSON_STRING("capture_mode", entry->config_copy.capture_mode);
    ADD_JSON_STRING("output_format", entry->config_copy.output_format);
    ADD_JSON_STRING("device_path", entry->config_copy.device_path);
    ADD_JSON_STRING("uri", entry->config_copy.uri);
    ADD_JSON_STRING("text", entry->config_copy.text);
    ADD_JSON_STRING("font_family", entry->config_copy.font_family);
    ADD_JSON_STRING("font_path", entry->config_copy.font_path);
    ADD_JSON_STRING("text_color", entry->config_copy.text_color);
    ADD_JSON_STRING("text_align", entry->config_copy.text_align);
    if (entry->config_copy.font_size > 0)
        g_string_append_printf(json, ",\"font_size\":%u", entry->config_copy.font_size);
    if (entry->config_copy.loop)
        g_string_append(json, ",\"loop\":true");
    g_string_append_c(json, '}');

#undef ADD_JSON_STRING
    return g_string_free(json, FALSE);
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
 * Fork/exec the source worker process.
 */
static int source_entry_launch(sbs_source_entry_t *entry)
{
    sbs_source_supervisor_t *sup = entry->supervisor;

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
              "--mode", "source",
              "--id", entry->source_id,
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
        LOG_W("partial config write to worker stdin (%zd/%zu)",
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
        if (entry->accept_timer_id) {
            g_source_remove(entry->accept_timer_id);
            entry->accept_timer_id = 0;
        }

        /* Install I/O watch for frame reception */
        entry->io_watch_id = g_unix_fd_add(entry->conn_fd,
                                            G_IO_IN | G_IO_HUP | G_IO_ERR,
                                            on_frame_available, entry);

        /* Start stable timer */
        entry->stable_timer_id = g_timeout_add(STABLE_RESET_MS,
                                                on_stable_timer, entry);

        LOG_D("source '%s' connected immediately", entry->source_id);
    } else {
        /* Install I/O watch on listening socket for deferred accept */
        entry->io_watch_id = g_unix_fd_add(entry->listen_fd,
                                            G_IO_IN,
                                            on_frame_available, entry);
    }

    return SBS_OK;
}

/**
 * Clean up worker process state without freeing the entry.
 * Used before restart.
 */
static void source_entry_cleanup_worker(sbs_source_entry_t *entry)
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
}

/* ── Frame Reception ──────────────────────────────────────────── */

/**
 * Handle incoming data on either the listening socket (deferred accept)
 * or the connected socket (frame/control message reception).
 */
static gboolean on_frame_available(gint fd, GIOCondition cond, gpointer user_data)
{
    sbs_source_entry_t *entry = user_data;

    /* If this is the listening socket waiting for accept */
    if (entry->conn_fd < 0 && fd == entry->listen_fd) {
        int conn_fd = try_accept_connection(entry->listen_fd);
        if (conn_fd < 0) {
            return G_SOURCE_CONTINUE;
        }

        entry->conn_fd = conn_fd;

        /* Cancel accept timeout */
        if (entry->accept_timer_id) {
            g_source_remove(entry->accept_timer_id);
            entry->accept_timer_id = 0;
        }

        /* Re-install I/O watch on the connected socket instead */
        entry->io_watch_id = g_unix_fd_add(entry->conn_fd,
                                            G_IO_IN | G_IO_HUP | G_IO_ERR,
                                            on_frame_available, entry);

        /* Start stable timer */
        entry->stable_timer_id = g_timeout_add(STABLE_RESET_MS,
                                                on_stable_timer, entry);

        LOG_I("source '%s' worker connected", entry->source_id);

        /* Return REMOVE for the old watch (on listen_fd) */
        return G_SOURCE_REMOVE;
    }

    /* Handle error/hangup conditions */
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        LOG_W("source '%s' connection lost (HUP/ERR)", entry->source_id);
        close(entry->conn_fd);
        entry->conn_fd = -1;
        entry->io_watch_id = 0;
        if (!entry->shutting_down && entry->pid > 0) {
            kill(entry->pid, SIGKILL);
        }
        /* Child watch callback will handle restart */
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        union {
            sbs_video_frame_msg_t    frame;
            sbs_status_msg_t         status;
            sbs_signal_change_msg_t  signal_change;
            uint8_t                  raw[512];
        } msg_buf;
        int dmabuf_fd = -1;
        int dmabuf_fd2 = -1;
        int rc;
        uint32_t msg_type;

        memset(&msg_buf, 0, sizeof(msg_buf));
        rc = sbs_ipc_recv_frame2(entry->conn_fd, &msg_buf.frame, &dmabuf_fd, &dmabuf_fd2);

        if (rc == SBS_ERR_WOULD_BLOCK) {
            break;
        }

        if (rc == SBS_ERR_EOF) {
            LOG_D("source '%s' connection closed (EOF)", entry->source_id);
            close(entry->conn_fd);
            entry->conn_fd = -1;
            entry->io_watch_id = 0;
            if (!entry->shutting_down && entry->pid > 0) {
                kill(entry->pid, SIGKILL);
            }
            return G_SOURCE_REMOVE;
        }

        if (rc != SBS_OK) {
            LOG_W("source '%s' recv error: %d", entry->source_id, rc);
            break;
        }

        msg_type = msg_buf.frame.header.msg_type;

        if (msg_type == SBS_IPC_MSG_VIDEO_FRAME) {

            entry->last_frame_time = g_get_monotonic_time();

            if (entry->muted) {
                send_source_frame_release(entry->conn_fd, &msg_buf.frame);
                if (dmabuf_fd >= 0) close(dmabuf_fd);
                if (dmabuf_fd2 >= 0) close(dmabuf_fd2);
                continue;
            }

            if (entry->supervisor->scene_graph) {
                sbs_source_state_t *src = sbs_scene_graph_get_source(
                    entry->supervisor->scene_graph, entry->source_id);
                if (src) {
                    bool scene_changed = false;
                    if (src->frame_width != msg_buf.frame.width ||
                        src->frame_height != msg_buf.frame.height) {
                        src->frame_width = msg_buf.frame.width;
                        src->frame_height = msg_buf.frame.height;
                        src->slot_initialized = true;
                        scene_changed = true;
                    }
                    if (msg_buf.frame.drm_format != 0 &&
                        src->drm_format != msg_buf.frame.drm_format) {
                        src->drm_format = msg_buf.frame.drm_format;
                        scene_changed = true;
                    }
                    if (src->drm_modifier != msg_buf.frame.drm_modifier) {
                        src->drm_modifier = msg_buf.frame.drm_modifier;
                        scene_changed = true;
                    }
                    if (src->plane_stride[0] != msg_buf.frame.plane_stride[0] ||
                        src->plane_stride[1] != msg_buf.frame.plane_stride[1]) {
                        scene_changed = true;
                    }
                    {
                        uint32_t next_depth = msg_buf.frame.drm_format == DRM_FORMAT_P010 ? 10 : 8;
                        bool next_hdr = (msg_buf.frame.flags & SBS_FRAME_FLAG_HDR) != 0;
                        if (src->color_depth != next_depth || src->hdr != next_hdr) {
                            src->color_depth = next_depth;
                            src->hdr = next_hdr;
                            g_strlcpy(src->color_space, next_hdr ? "BT.2020" : "BT.709",
                                      sizeof(src->color_space));
                            g_strlcpy(src->hdr_eotf, next_hdr ? "PQ" : "SDR",
                                      sizeof(src->hdr_eotf));
                            scene_changed = true;
                        }
                    }
                    src->plane_offset[0] = msg_buf.frame.plane_offset[0];
                    src->plane_offset[1] = msg_buf.frame.plane_offset[1];
                    src->plane_stride[0] = msg_buf.frame.plane_stride[0];
                    src->plane_stride[1] = msg_buf.frame.plane_stride[1];
                    if (scene_changed && entry->supervisor->api_server) {
                        sbs_api_server_refresh_scene(entry->supervisor->api_server);
                    }
                }
            }

            sbs_frame_fds_t *fds = calloc(1, sizeof(*fds));
            if (!fds) {
                send_source_frame_release(entry->conn_fd, &msg_buf.frame);
                if (dmabuf_fd >= 0) close(dmabuf_fd);
                if (dmabuf_fd2 >= 0 && dmabuf_fd2 != dmabuf_fd) close(dmabuf_fd2);
                continue;
            }
            fds->release_sock_fd = -1;
            fds->dmabuf_fd = dmabuf_fd;
            fds->dmabuf_fd2 = dmabuf_fd2;
            if (msg_buf.frame.flags & SBS_FRAME_FLAG_NEEDS_RELEASE) {
                fds->release_sock_fd = dup(entry->conn_fd);
                if (fds->release_sock_fd < 0) {
                    LOG_W("source '%s' failed to duplicate release socket", entry->source_id);
                    send_source_frame_release(entry->conn_fd, &msg_buf.frame);
                    sbs_frame_fds_release(fds, NULL);
                    continue;
                }
                fds->release_sequence = msg_buf.frame.sequence;
                fds->needs_release_ack = true;
            }

            sbs_frame_slot_publish(entry->frame_slot, fds,
                                   (uint64_t)entry->last_frame_time);

            LOG_T("source '%s' frame %lu (fd=%d fd2=%d)",
                  entry->source_id, (unsigned long)msg_buf.frame.sequence,
                  dmabuf_fd, dmabuf_fd2);
        } else if (msg_type == SBS_IPC_MSG_STATUS) {
            sbs_worker_state_t wstate = (sbs_worker_state_t)msg_buf.status.state;
            LOG_T("source '%s' status: state=%u frames=%lu dropped=%lu",
                  entry->source_id, wstate,
                  (unsigned long)msg_buf.status.frames_produced,
                  (unsigned long)msg_buf.status.frames_dropped);

            if (entry->supervisor->scene_graph) {
                sbs_source_state_t *src = sbs_scene_graph_get_source(
                    entry->supervisor->scene_graph, entry->source_id);
                if (src) {
                    const char *new_state = NULL;
                    if (wstate == SBS_WORKER_STATE_RUNNING &&
                        g_strcmp0(src->runtime_state, "running") != 0) {
                        new_state = "running";
                    } else if (wstate == SBS_WORKER_STATE_ERROR) {
                        new_state = "error";
                        g_free(src->error_message);
                        src->error_message = g_strdup(msg_buf.status.error_message);
                    }
                    if (new_state) {
                        g_free(src->runtime_state);
                        src->runtime_state = g_strdup(new_state);
                        if (entry->supervisor->api_server) {
                            sbs_api_server_publish(
                                entry->supervisor->api_server,
                                "source.status",
                                sbs_scene_graph_serialize_source(src));
                        }
                    }
                }
            }
        } else if (msg_type == SBS_IPC_MSG_SIGNAL_CHANGE) {
            LOG_I("source '%s' signal change: %s (%ux%u)",
                  entry->source_id, msg_buf.signal_change.reason,
                  msg_buf.signal_change.width, msg_buf.signal_change.height);
            if (entry->supervisor->scene_graph) {
                sbs_source_state_t *src = sbs_scene_graph_get_source(
                    entry->supervisor->scene_graph, entry->source_id);
                if (src) {
                    if (msg_buf.signal_change.width > 0)
                        src->frame_width = msg_buf.signal_change.width;
                    if (msg_buf.signal_change.height > 0)
                        src->frame_height = msg_buf.signal_change.height;
                    src->color_depth = msg_buf.signal_change.color_depth > 0
                        ? msg_buf.signal_change.color_depth
                        : 8;
                    src->hdr = g_strcmp0(msg_buf.signal_change.hdr_eotf, "SDR") != 0;
                    g_strlcpy(src->color_space,
                              msg_buf.signal_change.color_space,
                              sizeof(src->color_space));
                    g_strlcpy(src->hdr_eotf,
                              msg_buf.signal_change.hdr_eotf,
                              sizeof(src->hdr_eotf));
                    src->slot_initialized = true;
                    if (entry->supervisor->api_server)
                        sbs_api_server_refresh_scene(entry->supervisor->api_server);
                }
            }
        } else {
            LOG_D("source '%s' unknown msg type 0x%04x", entry->source_id, msg_type);
            if (dmabuf_fd >= 0) close(dmabuf_fd);
            if (dmabuf_fd2 >= 0 && dmabuf_fd2 != dmabuf_fd) close(dmabuf_fd2);
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ── Child Exit Handler ───────────────────────────────────────── */

static void on_child_exit(GPid pid, gint status, gpointer user_data)
{
    sbs_source_entry_t *entry = user_data;
    entry->child_watch_id = 0;

    g_spawn_close_pid(pid);

    if (WIFEXITED(status)) {
        LOG_I("source '%s' (pid=%d) exited with code %d",
              entry->source_id, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LOG_W("source '%s' (pid=%d) killed by signal %d",
              entry->source_id, (int)pid, WTERMSIG(status));
    }

    entry->pid = 0;

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
        LOG_D("source '%s' not restarting (shutting down)", entry->source_id);
        return;
    }

    /* Schedule restart with backoff */
    schedule_restart(entry);
}

/* ── Accept Timeout ───────────────────────────────────────────── */

static gboolean on_accept_timeout(gpointer user_data)
{
    sbs_source_entry_t *entry = user_data;
    entry->accept_timer_id = 0;

    LOG_W("source '%s' worker did not connect within %d ms",
          entry->source_id, ACCEPT_TIMEOUT_MS);

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
    sbs_source_entry_t *entry = user_data;
    entry->stable_timer_id = 0;

    if (entry->restart_attempts > 0) {
        LOG_I("source '%s' stable for %d s, resetting restart counter (was %u)",
              entry->source_id, STABLE_RESET_MS / 1000,
              entry->restart_attempts);
        entry->restart_attempts = 0;
    }

    return G_SOURCE_REMOVE;
}

/* ── Exponential Backoff Restart ──────────────────────────────── */

static void schedule_restart(sbs_source_entry_t *entry)
{
    entry->restart_attempts++;

    if (entry->restart_attempts > MAX_RESTART_ATTEMPTS) {
        LOG_E("source '%s' exceeded max restart attempts (%d), giving up",
              entry->source_id, MAX_RESTART_ATTEMPTS);
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

    /* Add jitter: ±25% */
    uint32_t jitter = (uint32_t)(g_random_int_range(0, (gint32)(delay_ms / 4)));
    if (g_random_boolean()) {
        delay_ms += jitter;
    } else if (delay_ms > jitter) {
        delay_ms -= jitter;
    }

    LOG_I("source '%s' scheduling restart in %u ms (attempt %u/%d)",
          entry->source_id, delay_ms, entry->restart_attempts,
          MAX_RESTART_ATTEMPTS);

    g_timeout_add(delay_ms, on_restart_timer, entry);
}

static gboolean on_restart_timer(gpointer user_data)
{
    sbs_source_entry_t *entry = user_data;

    if (entry->shutting_down) {
        LOG_D("source '%s' skipping restart (shutting down)", entry->source_id);
        return G_SOURCE_REMOVE;
    }

    LOG_I("restarting source '%s' (attempt %u)", entry->source_id,
          entry->restart_attempts);

    /* Clean up previous worker state */
    source_entry_cleanup_worker(entry);

    /* Relaunch */
    int rc = source_entry_launch(entry);
    if (rc != SBS_OK) {
        LOG_E("source '%s' relaunch failed: %d", entry->source_id, rc);
        /* Schedule another attempt */
        schedule_restart(entry);
    }

    return G_SOURCE_REMOVE;
}
