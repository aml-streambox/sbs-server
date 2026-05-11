#ifndef SBS_PREVIEW_H
#define SBS_PREVIEW_H

#include "cjson/cJSON.h"
#include "sbs/ipc.h"

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct sbs_preview_engine sbs_preview_engine_t;

typedef enum sbs_preview_color_mode {
    SBS_PREVIEW_COLOR_MODE_SDR = 0,
    SBS_PREVIEW_COLOR_MODE_HDR10 = 1,
} sbs_preview_color_mode_t;

typedef enum sbs_preview_profile_kind {
    SBS_PREVIEW_PROFILE_KIND_REUSE = 0,
    SBS_PREVIEW_PROFILE_KIND_FALLBACK = 1,
} sbs_preview_profile_kind_t;

typedef struct sbs_preview_profile {
    char *id;
    sbs_preview_profile_kind_t kind;
    char *transport;
    char *codec;
    char *container;
    char *latency_class;
    uint32_t width;
    uint32_t height;
    uint32_t downscale_factor;
    uint32_t framerate;
    bool hardware_decode_preferred;
    bool requires_additional_encode;
    bool available;
    bool requestable;
    bool active;
    uint32_t viewer_count;
    char *stream_url;
    uint32_t bitrate_kbps;
} sbs_preview_profile_t;

typedef struct sbs_preview_telemetry {
    uint32_t active_sessions;
    uint64_t total_sessions_created;
    uint64_t total_frames_produced;
    uint64_t total_frames_dropped;
    double avg_latency_ms;
    bool degraded;
} sbs_preview_telemetry_t;

/* Callback invoked when a new ICE candidate is generated.
 * userdata is the value passed to sbs_preview_engine_set_ice_callback(). */
typedef void (*sbs_preview_ice_callback_t)(void *userdata,
                                           unsigned int mline_index,
                                           const char *candidate);

sbs_preview_engine_t *sbs_preview_engine_new(void);
void sbs_preview_engine_free(sbs_preview_engine_t *engine);

void sbs_preview_engine_set_api_port(sbs_preview_engine_t *engine, uint16_t port);
void sbs_preview_engine_set_source_format(sbs_preview_engine_t *engine,
                                            uint32_t width, uint32_t height, uint32_t fps,
                                            sbs_preview_color_mode_t color_mode);
void sbs_preview_engine_set_ice_callback(sbs_preview_engine_t *engine,
                                         sbs_preview_ice_callback_t callback,
                                         void *userdata);

sbs_preview_profile_t *sbs_preview_engine_active_fallback(const sbs_preview_engine_t *engine);
sbs_preview_profile_t *sbs_preview_engine_get_profile(const sbs_preview_engine_t *engine,
                                                      const char *profile_id);
int sbs_preview_engine_ensure_profile(sbs_preview_engine_t *engine,
                                      const char *profile_id,
                                      sbs_preview_profile_t **out_profile);
int sbs_preview_engine_release_profile(sbs_preview_engine_t *engine,
                                       const char *profile_id,
                                       sbs_preview_profile_t **out_profile);
/* sbs_preview_engine_consume_frame — REMOVED (was fd-based memfd path) */
void sbs_preview_engine_consume_frame_ptr(sbs_preview_engine_t *engine,
                                            const sbs_video_frame_msg_t *msg,
                                            const void *data,
                                            size_t size);
void sbs_preview_engine_consume_frame_dmabuf(sbs_preview_engine_t *engine,
                                              const sbs_video_frame_msg_t *msg,
                                              int dmabuf_fd,
                                              size_t size);
void sbs_preview_engine_consume_audio(sbs_preview_engine_t *engine,
                                      const sbs_audio_buffer_msg_t *msg,
                                      const void *data);
void sbs_preview_engine_update_metrics(sbs_preview_engine_t *engine,
                                        uint64_t produced,
                                       uint64_t dropped,
                                       double latency_ms,
                                       bool degraded);
void sbs_preview_engine_collect_telemetry(const sbs_preview_engine_t *engine,
                                          sbs_preview_telemetry_t *telemetry);

/* WebRTC signaling */
int sbs_preview_engine_webrtc_start(sbs_preview_engine_t *engine,
                                    const char *profile_id,
                                    sbs_preview_profile_t **out_profile,
                                    const char **out_sdp,
                                    cJSON **out_ice_candidates);
int sbs_preview_engine_set_webrtc_answer(sbs_preview_engine_t *engine,
                                         const char *profile_id,
                                         const char *sdp_answer);
int sbs_preview_engine_add_webrtc_ice(sbs_preview_engine_t *engine,
                                      const char *profile_id,
                                      unsigned int mline_index,
                                      const char *candidate);

int sbs_preview_engine_update_profile_config(sbs_preview_engine_t *engine,
                                               const char *profile_id,
                                               uint32_t downscale_factor,
                                               uint32_t framerate,
                                               uint32_t bitrate_kbps);

cJSON *sbs_preview_serialize_profile(const sbs_preview_profile_t *profile);
cJSON *sbs_preview_serialize_profile_catalog(const sbs_preview_engine_t *engine);
cJSON *sbs_preview_serialize_telemetry(const sbs_preview_engine_t *engine);

#endif
