/*
 * SBS - StreamBox Broadcast System
 * Worker common infrastructure — shared setup for source and output workers
 *
 * Handles: JSON config parsing from stdin, IPC socket connection,
 * GStreamer initialization, signal handling for clean shutdown.
 */
#ifndef SBS_WORKER_COMMON_H
#define SBS_WORKER_COMMON_H

#include "sbs/ipc.h"
#include "sbs/ipc_transport.h"
#include "sbs/types.h"

#include <glib.h>
#include <gst/gst.h>
#include <cjson/cJSON.h>

/* ── Worker Configuration ─────────────────────────────────────── */

/**
 * Source-specific configuration fields.
 */
typedef struct sbs_source_config {
    char     *source_type;      /* "vfmcap", "videotestsrc", "v4l2src", "uridecodebin", "image", "text" */
    char     *capture_mode;     /* vfmcap color mode */
    char     *output_format;    /* vfmcap output format */
    char     *pattern;          /* "smpte", "snow", etc. (videotestsrc only) */
    char     *device_path;      /* "/dev/videoN" (v4l2src only) */
    char     *format;           /* V4L2 fourcc, e.g. "NV12", "MJPG" */
    char     *framerate;        /* V4L2 rate, e.g. "30/1" */
    char     *decode_mode;      /* "auto", "hardware", "software" */
    char     *uri;              /* file/network URI (uridecodebin only) */
    bool      loop;             /* Loop media file on EOS */
    char     *text;             /* Text source content */
    char     *font_family;      /* Text source font family */
    char     *font_path;        /* Optional uploaded font file */
    char     *text_color;       /* #RRGGBB or #RRGGBBAA */
    char     *text_align;       /* left, center, right */
    uint32_t  font_size;        /* Text source font size in pixels */
    cJSON    *properties;       /* Additional GStreamer element properties (borrowed from config root) */
} sbs_source_config_t;

/**
 * Output-specific configuration fields.
 */
typedef struct sbs_output_config {
    char     *codec;            /* "h265", "h264" */
    uint32_t  bitrate;          /* Target bitrate in kbps */
    char     *sink_type;        /* "srt", "rtmp", "file", "fakesink" */
    char     *srt_uri;          /* SRT listener/caller URI */
    char     *srt_mode;         /* "listener"/"server" or "caller"/"client" */
    char     *srt_stream_key;   /* Optional SRT streamid/stream key */
    char     *srt_passphrase;   /* Optional SRT encryption passphrase */
    uint32_t  srt_latency_ms;  /* SRT latency (default: 600) */
    char     *rtmp_uri;         /* RTMP URI (e.g. "rtmp://server/live/key") */
    char     *rtmp_passcode;    /* Optional RTMP stream key */
    char     *rtmp_plugin;      /* "streambox" (default) or "legacy" for stock flvmux+rtmpsink */
    char     *file_path;        /* File output path (e.g. "/tmp/stream.ts") */
    char     *file_path_mode;   /* "file" or "directory" */
    char     *file_prefix;      /* Directory mode filename prefix */
    char     *file_container;   /* "ts", "mkv", "flv", or "mp4" */
    char     *encoder;          /* Override encoder element name (NULL = auto) */
    uint32_t  gop_size;         /* Keyframe interval in frames */
} sbs_output_config_t;

/**
 * Unified worker configuration parsed from JSON on stdin.
 */
typedef struct sbs_worker_config {
    /* Common fields */
    char     *mode;             /* "source" or "output" */
    char     *worker_id;        /* Source/output UUID */
    char     *socket_path;      /* IPC socket path */
    uint32_t  width;
    uint32_t  height;
    uint32_t  framerate_num;    /* Framerate numerator */
    uint32_t  framerate_den;    /* Framerate denominator (default 1) */
    uint32_t  heartbeat_interval_ms;  /* Heartbeat period (default 5000) */

    /* Mode-specific config */
    union {
        sbs_source_config_t source;
        sbs_output_config_t output;
    };

    /* The raw cJSON tree — freed in worker_config_free() */
    cJSON    *_root;
} sbs_worker_config_t;

/* ── Worker Context (shared runtime state) ────────────────────── */

/**
 * Shared worker runtime state.
 *
 * Created by sbs_worker_init(), used by source/output implementations,
 * freed by sbs_worker_cleanup().
 */
typedef struct sbs_worker_ctx {
    sbs_worker_config_t  config;
    int                  sock_fd;    /* Connected IPC socket to supervisor */
    GMainLoop           *loop;       /* GLib main loop (quit on shutdown) */
    volatile bool        shutting_down;  /* Set by signal handler */
} sbs_worker_ctx_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Read JSON configuration from stdin (blocks until EOF).
 *
 * The supervisor writes config JSON to the worker's stdin pipe and closes it.
 * Returns a heap-allocated string; caller must free().
 *
 * @return JSON string on success, NULL on read error or empty input
 */
char *sbs_worker_read_stdin(void);

/**
 * Parse a JSON config string into a worker_config_t.
 *
 * @param json_str  Null-terminated JSON string
 * @param config    Output config struct (zero-initialized by caller)
 * @return SBS_OK on success, SBS_ERR_INVAL on parse error
 */
int sbs_worker_config_parse(const char *json_str, sbs_worker_config_t *config);

/**
 * Free all resources owned by a worker config.
 */
void sbs_worker_config_free(sbs_worker_config_t *config);

/**
 * Connect to the supervisor's Unix domain socket.
 *
 * @param socket_path  Path to the supervisor's listening socket
 * @return Connected fd on success, -1 on error (logs internally)
 */
int sbs_worker_connect(const char *socket_path);

/**
 * Initialize the shared worker context.
 *
 * Performs: GStreamer init, config parse, IPC connect, signal handler setup.
 * On success, ctx is fully initialized and ready for source/output dispatch.
 *
 * @param ctx         Output context (caller allocates)
 * @param argc        Pointer to argc (for gst_init)
 * @param argv        Pointer to argv (for gst_init)
 * @param mode        "source" or "output"
 * @param worker_id   Worker UUID string
 * @param socket_path IPC socket path
 * @return SBS_OK on success, negative error code on failure
 */
int sbs_worker_init(sbs_worker_ctx_t *ctx, int *argc, char ***argv,
                    const char *mode, const char *worker_id,
                    const char *socket_path);

/**
 * Run the GLib main loop (blocks until quit).
 */
void sbs_worker_run(sbs_worker_ctx_t *ctx);

/**
 * Request shutdown (can be called from signal handler context).
 */
void sbs_worker_request_shutdown(sbs_worker_ctx_t *ctx);

/**
 * Clean up all worker resources.
 *
 * Closes IPC socket, unrefs main loop, frees config.
 */
void sbs_worker_cleanup(sbs_worker_ctx_t *ctx);

/**
 * Send a status heartbeat to the supervisor.
 */
int sbs_worker_send_status(sbs_worker_ctx_t *ctx, sbs_worker_state_t state);

#endif /* SBS_WORKER_COMMON_H */
