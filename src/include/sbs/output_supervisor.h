/*
 * SBS - StreamBox Broadcast System
 * Output Supervisor — manages output worker processes
 *
 * Handles fork/exec of sbs-worker --mode=output, composed frame delivery,
 * crash detection with exponential backoff recovery, and graceful shutdown.
 *
 * Reference: document/06-output-manager.md sections 2, 6-8
 */
#ifndef SBS_OUTPUT_SUPERVISOR_H
#define SBS_OUTPUT_SUPERVISOR_H

#include "sbs/types.h"
#include "sbs/ipc.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Opaque Types ─────────────────────────────────────────────── */

typedef struct sbs_output_supervisor sbs_output_supervisor_t;

/* ── Output Configuration ─────────────────────────────────────── */

/**
 * Configuration for starting an output worker.
 *
 * The supervisor serializes this to JSON and pipes it to the worker's stdin.
 */
typedef struct sbs_output_start_config {
    const char *output_id;       /* Unique output ID */

    /* Video input format (composed frame properties) */
    uint32_t    width;
    uint32_t    height;
    uint32_t    framerate_num;
    uint32_t    framerate_den;

    /* Encoder settings */
    const char *codec;           /* "h264" or "h265" (default: "h265") */
    uint32_t    bitrate_kbps;    /* Target bitrate (default: 20000) */
    uint32_t    gop_size;        /* Keyframe interval in frames (0 = auto) */
    const char *encoder;         /* Encoder override (NULL = auto-select) */

    /* Output sink */
    const char *sink_type;       /* "srt", "rtmp", "file", "fakesink" */
    const char *srt_uri;         /* SRT URI (e.g. "srt://:8888") */
    uint32_t    srt_latency_ms;  /* SRT latency (default: 600) */
    const char *rtmp_uri;        /* RTMP URI (e.g. "rtmp://server/live/key") */
    const char *rtmp_passcode;   /* Optional RTMP stream key/passcode */
    const char *file_path;       /* File output path (e.g. "/tmp/stream.ts") */
} sbs_output_start_config_t;

typedef struct sbs_output_supervisor_metrics {
    uint32_t total_outputs;
    uint32_t connected_outputs;
    uint32_t restarting_outputs;
    uint32_t max_restart_attempts;
    uint64_t frames_sent;
    uint64_t frames_dropped;
    bool degraded;
} sbs_output_supervisor_metrics_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Create a new output supervisor.
 *
 * @param worker_path  Path to sbs-worker binary (e.g. "/usr/bin/sbs-worker")
 * @param sock_dir     Directory for IPC sockets (e.g. "/run/sbs")
 * @return New supervisor, or NULL on allocation failure
 */
sbs_output_supervisor_t *sbs_output_supervisor_new(const char *worker_path,
                                                    const char *sock_dir);

/**
 * Free the supervisor and all owned resources.
 *
 * If any workers are still running, they are killed (SIGKILL).
 * All sockets are closed and unlinked.
 */
void sbs_output_supervisor_free(sbs_output_supervisor_t *sup);

/**
 * Start an output worker.
 *
 * Creates a Unix socket at {sock_dir}/output-{id}.sock, fork/execs
 * sbs-worker --mode=output, pipes JSON config to stdin, accepts the
 * connection, and prepares for frame delivery.
 *
 * @param sup     Supervisor instance
 * @param config  Output start configuration
 * @return SBS_OK on success, negative error code on failure
 */
int sbs_output_supervisor_start_output(sbs_output_supervisor_t *sup,
                                        const sbs_output_start_config_t *config);

/**
 * Stop an output worker by ID.
 *
 * Sends SBS_MSG_SHUTDOWN, waits for clean exit, escalates to SIGKILL if needed.
 *
 * @param sup        Supervisor instance
 * @param output_id  Output ID to stop
 * @return SBS_OK on success, SBS_ERR_NOT_FOUND if output not found
 */
int sbs_output_supervisor_stop_output(sbs_output_supervisor_t *sup,
                                       const char *output_id);

/**
 * Shutdown all output workers.
 *
 * Implements the 4-stage shutdown sequence:
 * Stage 0: Send SBS_MSG_SHUTDOWN to each worker
 * Stage 1: Wait up to 2s for clean exit
 * Stage 2: SIGTERM remaining workers
 * Stage 3: SIGKILL after additional 1s
 *
 * Blocks until all workers have exited.
 *
 * @param sup  Supervisor instance
 */
void sbs_output_supervisor_shutdown_all(sbs_output_supervisor_t *sup);

/**
 * Send a composed frame to all active output workers.
 *
 * Uses MSG_DONTWAIT so a slow consumer never stalls the caller.
 * If a send would block, the frame is dropped for that output and
 * frames_dropped is incremented.
 *
 * The caller retains ownership of the dmabuf_fd — the supervisor dup()s
 * it for each output worker that accepts the frame.
 *
 * @param sup        Supervisor instance
 * @param msg        Composed frame message (header + metadata)
 * @param dmabuf_fd  DMA-BUF fd of the composed frame
 * @return Number of outputs that successfully received the frame
 */
int sbs_output_supervisor_send_frame(sbs_output_supervisor_t *sup,
                                      const sbs_video_frame_msg_t *msg,
                                      int dmabuf_fd);

int sbs_output_supervisor_send_audio(sbs_output_supervisor_t *sup,
                                     const sbs_audio_buffer_msg_t *msg,
                                     const void *audio_data);

/**
 * Get number of active outputs.
 */
uint32_t sbs_output_supervisor_output_count(const sbs_output_supervisor_t *sup);

void sbs_output_supervisor_get_metrics(const sbs_output_supervisor_t *sup,
                                       sbs_output_supervisor_metrics_t *metrics);

#endif /* SBS_OUTPUT_SUPERVISOR_H */
