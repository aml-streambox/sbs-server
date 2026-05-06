#include "sbs/compositor_thread.h"
#include "sbs/encoder_manager.h"
#include "sbs/output_router.h"
#include "sbs/source_supervisor.h"
#include "sbs/output_supervisor.h"

#include <string.h>

static char last_source_output_format[32];
static int source_start_result;
static unsigned source_start_count;

void test_api_stubs_reset_last_source_start(void)
{
    last_source_output_format[0] = '\0';
    source_start_result = 0;
    source_start_count = 0;
}

const char *test_api_stubs_last_source_output_format(void)
{
    return last_source_output_format[0] ? last_source_output_format : NULL;
}

void test_api_stubs_set_source_start_result(int result)
{
    source_start_result = result;
}

unsigned test_api_stubs_source_start_count(void)
{
    return source_start_count;
}

int sbs_compositor_thread_set_scene(sbs_compositor_thread_t *ct,
                                    const sbs_comp_scene_state_t *state)
{
    (void)ct;
    (void)state;
    return 0;
}

bool sbs_compositor_thread_can_export_dmabuf(sbs_compositor_thread_t *ct)
{
    (void)ct;
    return true;
}

void sbs_compositor_thread_get_stats(sbs_compositor_thread_t *ct,
                                      uint64_t *frame_count,
                                      uint32_t *frames_dropped)
{
    (void)ct;
    if (frame_count) {
        *frame_count = 120;
    }
    if (frames_dropped) {
        *frames_dropped = 0;
    }
}

void sbs_compositor_thread_get_timing(sbs_compositor_thread_t *ct,
                                      sbs_comp_timing_stats_t *out)
{
    (void)ct;
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->frame_count = 120;
    out->content_frame_count = 120;
    out->fps_actual = 60.0;
}

void sbs_compositor_thread_native_mailbox_get_stats(sbs_compositor_thread_t *ct,
                                                     sbs_native_canvas_mailbox_type_t type,
                                                     sbs_native_canvas_mailbox_stats_t *out)
{
    (void)ct;
    (void)type;
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
}

void sbs_compositor_thread_get_canvas_config(sbs_compositor_thread_t *ct,
                                             uint32_t *width,
                                             uint32_t *height,
                                             uint32_t *fps,
                                             sbs_export_color_mode_t *color_mode)
{
    (void)ct;
    if (width) *width = 3840;
    if (height) *height = 2160;
    if (fps) *fps = 60;
    if (color_mode) *color_mode = SBS_EXPORT_COLOR_SDR;
}

uint64_t sbs_output_router_frames_distributed(const sbs_output_router_t *router)
{
    (void)router;
    return 120;
}

void sbs_output_router_set_preview_engine(sbs_output_router_t *router,
                                          sbs_preview_engine_t *preview)
{
    (void)router;
    (void)preview;
}

void sbs_output_router_set_snapshot_engine(sbs_output_router_t *router,
                                           sbs_snapshot_engine_t *snapshot)
{
    (void)router;
    (void)snapshot;
}

void sbs_output_router_set_audio_mixer(sbs_output_router_t *router,
                                        sbs_audio_mixer_t *audio)
{
    (void)router;
    (void)audio;
}

void sbs_output_router_set_color_mode(sbs_output_router_t *router,
                                      sbs_export_color_mode_t color_mode)
{
    (void)router;
    (void)color_mode;
}

int sbs_source_supervisor_start_source(sbs_source_supervisor_t *sup,
                                        const sbs_source_start_config_t *config,
                                        sbs_frame_slot_t *slot)
{
    (void)sup;
    (void)slot;
    source_start_count++;
    g_strlcpy(last_source_output_format,
              config && config->output_format ? config->output_format : "",
              sizeof(last_source_output_format));
    return source_start_result;
}

int sbs_source_supervisor_stop_source(sbs_source_supervisor_t *sup,
                                       const char *source_id)
{
    (void)sup;
    (void)source_id;
    return 0;
}

int sbs_source_supervisor_mute_source(sbs_source_supervisor_t *sup,
                                      const char *source_id)
{
    (void)sup;
    (void)source_id;
    return 0;
}

int sbs_source_supervisor_unmute_source(sbs_source_supervisor_t *sup,
                                        const char *source_id)
{
    (void)sup;
    (void)source_id;
    return 0;
}

void sbs_source_supervisor_shutdown_all(sbs_source_supervisor_t *sup)
{
    (void)sup;
}

void sbs_source_supervisor_get_metrics(const sbs_source_supervisor_t *sup,
                                       sbs_source_supervisor_metrics_t *metrics)
{
    (void)sup;
    if (!metrics) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->total_sources = 1;
    metrics->connected_sources = 1;
}

int sbs_output_supervisor_start_output(sbs_output_supervisor_t *sup,
                                       const sbs_output_start_config_t *config)
{
    (void)sup;
    (void)config;
    return 0;
}

int sbs_output_supervisor_stop_output(sbs_output_supervisor_t *sup,
                                      const char *output_id)
{
    (void)sup;
    (void)output_id;
    return 0;
}

void sbs_output_supervisor_shutdown_all(sbs_output_supervisor_t *sup)
{
    (void)sup;
}

void sbs_output_supervisor_get_metrics(const sbs_output_supervisor_t *sup,
                                        sbs_output_supervisor_metrics_t *metrics)
{
    (void)sup;
    if (!metrics) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->total_outputs = 1;
    metrics->connected_outputs = 1;
    metrics->frames_sent = 120;
}

int sbs_encoder_manager_update_config(sbs_encoder_manager_t *mgr,
                                      const sbs_encoder_config_t *enc_config)
{
    (void)mgr;
    (void)enc_config;
    return 0;
}

int sbs_encoder_manager_add_sink(sbs_encoder_manager_t *mgr,
                                 const sbs_sink_branch_config_t *config)
{
    (void)mgr;
    (void)config;
    return 0;
}

int sbs_encoder_manager_remove_sink(sbs_encoder_manager_t *mgr,
                                    const char *output_id)
{
    (void)mgr;
    (void)output_id;
    return 0;
}

bool sbs_encoder_manager_has_sink(const sbs_encoder_manager_t *mgr,
                                  const char *output_id)
{
    (void)mgr;
    (void)output_id;
    return false;
}

void sbs_encoder_manager_get_metrics(const sbs_encoder_manager_t *mgr,
                                     sbs_encoder_manager_metrics_t *metrics)
{
    (void)mgr;
    if (!metrics) {
        return;
    }
    memset(metrics, 0, sizeof(*metrics));
}

void sbs_encoder_manager_get_config(const sbs_encoder_manager_t *mgr,
                                    sbs_encoder_config_t *out_config)
{
    (void)mgr;
    if (!out_config) {
        return;
    }
    memset(out_config, 0, sizeof(*out_config));
    out_config->codec = "h265";
    out_config->bitrate_kbps = 12000;
    out_config->gop_size = 60;
    out_config->gop_pattern = 1;
    out_config->rc_mode = 0;
    out_config->encoder = "wave521";
}

void sbs_encoder_manager_get_format(const sbs_encoder_manager_t *mgr,
                                    uint32_t *width,
                                    uint32_t *height,
                                    uint32_t *fps_num,
                                    uint32_t *fps_den)
{
    (void)mgr;
    if (width) *width = 3840;
    if (height) *height = 2160;
    if (fps_num) *fps_num = 60;
    if (fps_den) *fps_den = 1;
}
