/*
 * SBS - StreamBox Broadcast System
 * Encoder Manager — direct video encoder with GStreamer mux fanout
 *
 * Architecture:
 *   direct Wave521 encode → appsrc(encoded) → h265parse/h264parse → tee
 *                                                                ├→ queue → mux → sink (output 1)
 *                                                                ├→ queue → mux → sink (output 2)
 *                                                                └→ ...
 *
 * Audio:
 *   audio_appsrc(S16LE) → queue → audioconvert → audioresample → avenc_aac → aacparse → audio_tee
 *                                                                                         ├→ mux (output 1)
 *                                                                                         └→ ...
 *
 * Each output is a "sink branch" added/removed dynamically via tee request pads.
 * Runs in-process on the main GLib thread (same as output_router calls).
 */
#ifndef SBS_ENCODER_MANAGER_H
#define SBS_ENCODER_MANAGER_H

#include "sbs/types.h"
#include "sbs/ipc.h"
#include "cjson/cJSON.h"

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Opaque Types ─────────────────────────────────────────────── */

typedef struct sbs_encoder_manager sbs_encoder_manager_t;

/* ── Encoder Configuration ────────────────────────────────────── */

typedef struct sbs_encoder_config {
    const char *codec;
    uint32_t    bitrate_kbps;
    uint32_t    gop_size;
    int32_t     gop_pattern;
    int32_t     rc_mode;        /* 0=VBR (default), 1=CBR */
    const char *encoder;
    bool        hdr10;
} sbs_encoder_config_t;

/* ── Sink Branch Configuration ────────────────────────────────── */

typedef struct sbs_sink_branch_config {
    const char *output_id;       /* Unique output ID */
    const char *sink_type;       /* "srt", "rtmp", "file", "fakesink" */
    const char *srt_uri;         /* SRT URI (e.g. "srt://:8888") */
    uint32_t    srt_latency_ms;  /* SRT latency (default: 600) */
    const char *rtmp_uri;        /* RTMP URI */
    const char *rtmp_passcode;   /* Optional RTMP stream key */
    const char *file_path;       /* File output path */
    const char *file_path_mode;  /* "file" or "directory" */
    const char *file_prefix;     /* Directory mode filename prefix */
    const char *file_container;  /* "ts", "mkv", "flv", or "mp4" */
} sbs_sink_branch_config_t;

/* ── Metrics ──────────────────────────────────────────────────── */

typedef struct sbs_encoder_manager_metrics {
    uint64_t frames_pushed;
    uint64_t frames_dropped;
    uint64_t encoded_bytes;
    uint32_t active_branches;
    bool     pipeline_active;
} sbs_encoder_manager_metrics_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Create a new encoder manager.
 *
 * Does NOT start the GStreamer pipeline. Call _ensure_pipeline() or
 * _add_sink_branch() to lazily start it.
 *
 * @param width         Video width (from canvas)
 * @param height        Video height
 * @param fps_num       Framerate numerator
 * @param fps_den       Framerate denominator
 * @param enc_config    Encoder settings (codec, bitrate, gop)
 * @return New encoder manager, or NULL on failure
 */
sbs_encoder_manager_t *sbs_encoder_manager_new(uint32_t width,
                                                uint32_t height,
                                                uint32_t fps_num,
                                                uint32_t fps_den,
                                                const sbs_encoder_config_t *enc_config);

/**
 * Free the encoder manager and tear down the pipeline.
 */
void sbs_encoder_manager_free(sbs_encoder_manager_t *mgr);

/**
 * Update encoder settings. If the pipeline is running, it will be
 * torn down and rebuilt with the new settings. Active sink branches
 * are re-added automatically.
 *
 * @return SBS_OK on success
 */
int sbs_encoder_manager_update_config(sbs_encoder_manager_t *mgr,
                                       const sbs_encoder_config_t *enc_config);

void sbs_encoder_manager_consume_frame_dmabuf(sbs_encoder_manager_t *mgr,
                                              const sbs_video_frame_msg_t *msg,
                                              int dmabuf_fd,
                                              size_t size);

/**
 * Push an audio buffer to the shared audio encoder.
 */
void sbs_encoder_manager_consume_audio(sbs_encoder_manager_t *mgr,
                                        const sbs_audio_buffer_msg_t *msg,
                                        const void *audio_data);

/**
 * Add a sink branch to the tee.
 *
 * If the pipeline is not running, it will be started.
 * The branch consists of: queue → mux → sink
 *
 * @param mgr     Encoder manager
 * @param config  Sink branch configuration
 * @return SBS_OK on success, error code on failure
 */
int sbs_encoder_manager_add_sink(sbs_encoder_manager_t *mgr,
                                  const sbs_sink_branch_config_t *config);

/**
 * Remove a sink branch from the tee.
 *
 * If no branches remain, the pipeline is torn down so the hardware encoder is
 * released for Preview-only or external encoder users.
 *
 * @param mgr        Encoder manager
 * @param output_id  Output ID of the branch to remove
 * @return SBS_OK on success, SBS_ERR_NOT_FOUND if not found
 */
int sbs_encoder_manager_remove_sink(sbs_encoder_manager_t *mgr,
                                     const char *output_id);

/**
 * Check if a sink branch exists.
 */
bool sbs_encoder_manager_has_sink(const sbs_encoder_manager_t *mgr,
                                   const char *output_id);

/**
 * Get number of active sink branches.
 */
uint32_t sbs_encoder_manager_sink_count(const sbs_encoder_manager_t *mgr);

/**
 * Get metrics.
 */
void sbs_encoder_manager_get_metrics(const sbs_encoder_manager_t *mgr,
                                       sbs_encoder_manager_metrics_t *metrics);
cJSON *sbs_encoder_manager_serialize_output_health(const sbs_encoder_manager_t *mgr);

/**
 * Check if the encoder pipeline is active.
 */
bool sbs_encoder_manager_is_active(const sbs_encoder_manager_t *mgr);

/**
 * Get current encoder configuration (deep-copies strings into caller-provided struct).
 * Caller must NOT free the string pointers — they point into internal storage.
 */
void sbs_encoder_manager_get_config(const sbs_encoder_manager_t *mgr,
                                     sbs_encoder_config_t *out_config);

int sbs_encoder_manager_reconfigure_video(sbs_encoder_manager_t *mgr,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t fps_num,
                                           uint32_t fps_den,
                                           bool hdr10);

/**
 * Get the video resolution/fps being used.
 */
void sbs_encoder_manager_get_format(const sbs_encoder_manager_t *mgr,
                                     uint32_t *width, uint32_t *height,
                                     uint32_t *fps_num, uint32_t *fps_den);

#endif /* SBS_ENCODER_MANAGER_H */
