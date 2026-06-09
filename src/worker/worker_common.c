/*
 * SBS - StreamBox Broadcast System
 * Worker common infrastructure — shared setup for source and output workers
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "worker"

#include "worker_common.h"

#include "sbs/log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ── Static global for signal handler access ──────────────────── */

static sbs_worker_ctx_t *g_ctx = NULL;

/* ── Read JSON from stdin ─────────────────────────────────────── */

char *sbs_worker_read_stdin(void)
{
    /* Read all of stdin into a buffer.
     * The supervisor writes JSON then closes the pipe, so we read until EOF. */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (n == 0) break;  /* EOF */
        len += (size_t)n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
    }

    if (len == 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

/* ── JSON Config Parsing ──────────────────────────────────────── */

/* Helper: extract a string from cJSON, strdup it. Returns NULL if missing. */
static char *json_get_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return strdup(item->valuestring);
    }
    return NULL;
}

/* Helper: extract a uint32 from cJSON. Returns default_val if missing. */
static uint32_t json_get_uint32(const cJSON *root, const char *key, uint32_t default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        return (uint32_t)item->valuedouble;
    }
    return default_val;
}

/* Helper: extract a bool from cJSON. Returns default_val if missing. */
static bool json_get_bool(const cJSON *root, const char *key, bool default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

static int parse_source_config(const cJSON *root, sbs_worker_config_t *config)
{
    sbs_source_config_t *src = &config->source;

    src->source_type   = json_get_string(root, "source_type");
    src->capture_mode  = json_get_string(root, "capture_mode");
    src->output_format = json_get_string(root, "output_format");
    src->pattern       = json_get_string(root, "pattern");
    src->device_path   = json_get_string(root, "device_path");
    src->format        = json_get_string(root, "format");
    src->framerate     = json_get_string(root, "framerate");
    src->decode_mode   = json_get_string(root, "decode_mode");
    src->uri           = json_get_string(root, "uri");
    src->loop          = json_get_bool(root, "loop", false);
    src->text          = json_get_string(root, "text");
    src->font_family   = json_get_string(root, "font_family");
    src->font_path     = json_get_string(root, "font_path");
    src->text_color    = json_get_string(root, "text_color");
    src->text_align    = json_get_string(root, "text_align");
    src->font_size     = json_get_uint32(root, "font_size", 48);

    /* Properties is a borrowed reference into the cJSON tree (_root owns it) */
    src->properties    = cJSON_GetObjectItemCaseSensitive(root, "properties");

    if (!src->source_type) {
        LOG_E("config missing 'source_type'");
        return SBS_ERR_INVAL;
    }

    return SBS_OK;
}

static int parse_output_config(const cJSON *root, sbs_worker_config_t *config)
{
    sbs_output_config_t *out = &config->output;

    out->codec         = json_get_string(root, "codec");
    out->srt_uri       = json_get_string(root, "srt_uri");
    out->srt_mode      = json_get_string(root, "srt_mode");
    out->srt_stream_key = json_get_string(root, "srt_stream_key");
    if (!out->srt_stream_key)
        out->srt_stream_key = json_get_string(root, "srt_stream_id");
    out->srt_passphrase = json_get_string(root, "srt_passphrase");
    out->encoder       = json_get_string(root, "encoder");
    out->sink_type     = json_get_string(root, "sink_type");
    out->srt_latency_ms = json_get_uint32(root, "srt_latency_ms", 600);
    out->rtmp_uri      = json_get_string(root, "rtmp_uri");
    out->rtmp_passcode = json_get_string(root, "rtmp_passcode");
    out->rtmp_plugin   = json_get_string(root, "rtmp_plugin");
    out->file_path     = json_get_string(root, "file_path");
    out->file_path_mode = json_get_string(root, "file_path_mode");
    out->file_prefix   = json_get_string(root, "file_prefix");
    out->file_container = json_get_string(root, "file_container");
    out->bitrate       = json_get_uint32(root, "bitrate_kbps", 5000);
    out->gop_size      = json_get_uint32(root, "gop_size", 60);

    if (!out->codec) {
        LOG_E("config missing 'codec'");
        return SBS_ERR_INVAL;
    }

    return SBS_OK;
}

int sbs_worker_config_parse(const char *json_str, sbs_worker_config_t *config)
{
    if (!json_str || !config) return SBS_ERR_INVAL;

    memset(config, 0, sizeof(*config));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        LOG_E("JSON parse error near: %.40s", err ? err : "(unknown)");
        return SBS_ERR_INVAL;
    }

    config->_root = root;

    /* Common fields */
    config->mode        = json_get_string(root, "mode");
    config->worker_id   = json_get_string(root, "worker_id");
    config->socket_path = json_get_string(root, "socket_path");
    config->width       = json_get_uint32(root, "width", 1920);
    config->height      = json_get_uint32(root, "height", 1080);
    config->framerate_num = json_get_uint32(root, "framerate_num", 30);
    config->framerate_den = json_get_uint32(root, "framerate_den", 1);
    config->heartbeat_interval_ms = json_get_uint32(root, "heartbeat_interval_ms", 5000);

    /* Parse mode-specific section.
     * The mode may come from either the JSON or the command line.
     * The caller (main.c) overrides config->mode with the CLI --mode value. */
    const char *mode = config->mode;
    if (!mode) {
        /* Mode not in JSON; caller will set it from CLI. Parse both sections. */
        return SBS_OK;
    }

    if (strcmp(mode, "source") == 0) {
        return parse_source_config(root, config);
    } else if (strcmp(mode, "output") == 0) {
        return parse_output_config(root, config);
    }

    return SBS_OK;
}

void sbs_worker_config_free(sbs_worker_config_t *config)
{
    if (!config) return;

    free(config->mode);
    free(config->worker_id);
    free(config->socket_path);

    /* Source fields (safe to free even if we're an output worker — they'll be NULL) */
    free(config->source.source_type);
    free(config->source.capture_mode);
    free(config->source.output_format);
    free(config->source.pattern);
    free(config->source.device_path);
    free(config->source.format);
    free(config->source.framerate);
    free(config->source.decode_mode);
    free(config->source.uri);
    free(config->source.text);
    free(config->source.font_family);
    free(config->source.font_path);
    free(config->source.text_color);
    free(config->source.text_align);
    /* config->source.properties is borrowed from _root — do NOT free */

    /* Output fields */
    free(config->output.codec);
    free(config->output.srt_uri);
    free(config->output.srt_mode);
    free(config->output.srt_stream_key);
    free(config->output.srt_passphrase);
    free(config->output.encoder);
    free(config->output.sink_type);
    free(config->output.rtmp_uri);
    free(config->output.rtmp_passcode);
    free(config->output.rtmp_plugin);
    free(config->output.file_path);
    free(config->output.file_path_mode);
    free(config->output.file_prefix);
    free(config->output.file_container);

    if (config->_root) {
        cJSON_Delete(config->_root);
        config->_root = NULL;
    }

    memset(config, 0, sizeof(*config));
}

/* ── IPC Socket Connection ────────────────────────────────────── */

int sbs_worker_connect(const char *socket_path)
{
    if (!socket_path) return -1;

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_E("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("connect(%s) failed: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking for send operations */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Increase socket buffer sizes */
    int buf_size = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    LOG_I("connected to supervisor socket: %s (fd=%d)", socket_path, fd);
    return fd;
}

/* ── Signal Handling ──────────────────────────────────────────── */

static void on_signal(int sig)
{
    /* Async-signal-safe: only set flag and quit main loop */
    if (g_ctx) {
        g_ctx->shutting_down = true;
        if (g_ctx->loop && g_main_loop_is_running(g_ctx->loop)) {
            g_main_loop_quit(g_ctx->loop);
        }
    }

    (void)sig;
}

static void install_signal_handlers(sbs_worker_ctx_t *ctx)
{
    g_ctx = ctx;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;  /* No SA_RESTART — we want blocking calls to be interrupted */
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGPIPE — we handle EPIPE from send() */
    signal(SIGPIPE, SIG_IGN);
}

/* ── Worker Lifecycle ─────────────────────────────────────────── */

int sbs_worker_init(sbs_worker_ctx_t *ctx, int *argc, char ***argv,
                    const char *mode, const char *worker_id,
                    const char *socket_path)
{
    if (!ctx || !mode || !worker_id || !socket_path) return SBS_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->sock_fd = -1;

    /* 1. GStreamer init */
    gst_init(argc, argv);

    /* 2. Read JSON config from stdin */
    char *json_str = sbs_worker_read_stdin();
    if (!json_str) {
        LOG_W("no config on stdin — using defaults");
        /* Create minimal default config */
        json_str = strdup("{}");
        if (!json_str) return SBS_ERR_NOMEM;
    }

    /* 3. Parse config */
    int rc = sbs_worker_config_parse(json_str, &ctx->config);
    free(json_str);
    if (rc != SBS_OK) {
        LOG_E("failed to parse config: %d", rc);
        return rc;
    }

    /* Override mode/id/socket from CLI args (authoritative over JSON) */
    free(ctx->config.mode);
    ctx->config.mode = strdup(mode);
    free(ctx->config.worker_id);
    ctx->config.worker_id = strdup(worker_id);
    free(ctx->config.socket_path);
    ctx->config.socket_path = strdup(socket_path);

    /* If mode-specific config wasn't parsed (mode was absent from JSON),
     * parse it now with the correct mode. */
    if (ctx->config._root) {
        if (strcmp(mode, "source") == 0) {
            rc = parse_source_config(ctx->config._root, &ctx->config);
        } else if (strcmp(mode, "output") == 0) {
            rc = parse_output_config(ctx->config._root, &ctx->config);
        }
        /* Non-critical: missing fields get defaults in the worker impl */
        if (rc != SBS_OK) {
            LOG_W("mode-specific config parse returned %d (continuing with defaults)", rc);
        }
    }

    /* 4. Connect to supervisor IPC socket */
    ctx->sock_fd = sbs_worker_connect(socket_path);
    if (ctx->sock_fd < 0) {
        LOG_E("failed to connect to supervisor");
        sbs_worker_config_free(&ctx->config);
        return SBS_ERR_IO;
    }

    /* 5. Create GLib main loop */
    ctx->loop = g_main_loop_new(NULL, FALSE);

    /* 6. Install signal handlers */
    install_signal_handlers(ctx);

    LOG_I("worker initialized (mode=%s, id=%s)", mode, worker_id);
    return SBS_OK;
}

void sbs_worker_run(sbs_worker_ctx_t *ctx)
{
    if (!ctx || !ctx->loop) return;
    g_main_loop_run(ctx->loop);
}

void sbs_worker_request_shutdown(sbs_worker_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->shutting_down = true;
    if (ctx->loop && g_main_loop_is_running(ctx->loop)) {
        g_main_loop_quit(ctx->loop);
    }
}

void sbs_worker_cleanup(sbs_worker_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->sock_fd >= 0) {
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }

    if (ctx->loop) {
        g_main_loop_unref(ctx->loop);
        ctx->loop = NULL;
    }

    sbs_worker_config_free(&ctx->config);
    gst_deinit();

    g_ctx = NULL;
    LOG_I("worker cleanup complete");
}

/* ── Status Heartbeat ─────────────────────────────────────────── */

int sbs_worker_send_status(sbs_worker_ctx_t *ctx, sbs_worker_state_t state)
{
    if (!ctx || ctx->sock_fd < 0) return SBS_ERR_INVAL;

    sbs_status_msg_t msg;
    sbs_status_msg_init(&msg);
    msg.state = (uint32_t)state;

    return sbs_ipc_send_msg(ctx->sock_fd, &msg, sizeof(msg));
}
