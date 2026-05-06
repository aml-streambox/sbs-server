#include "sbs/source_start_config.h"

#include <glib.h>
#include <string.h>

static const char *source_config_string(sbs_source_state_t *source, const char *key)
{
    if (!source || !source->config || !key) return NULL;
    return g_hash_table_lookup(source->config, key);
}

static bool source_config_bool(sbs_source_state_t *source, const char *key, bool fallback)
{
    const char *value = source_config_string(source, key);
    if (!value) return fallback;
    return g_ascii_strcasecmp(value, "true") == 0 ||
           g_ascii_strcasecmp(value, "yes") == 0 ||
           strcmp(value, "1") == 0;
}

static uint32_t source_config_u32(sbs_source_state_t *source, const char *key,
                                  uint32_t fallback, uint32_t min_value,
                                  uint32_t max_value)
{
    const char *value = source_config_string(source, key);
    char *end = NULL;
    guint64 parsed;
    if (!value || !value[0]) return fallback;
    parsed = g_ascii_strtoull(value, &end, 10);
    if (end == value || parsed < min_value || parsed > max_value) return fallback;
    return (uint32_t)parsed;
}

static const char *canvas_vfmcap_output_format(const sbs_canvas_state_t *canvas)
{
    return canvas && canvas->color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? "p010" : "raw";
}

void sbs_source_start_config_fill(const sbs_canvas_state_t *canvas,
                                  sbs_source_state_t *source,
                                  sbs_source_start_config_t *cfg)
{
    uint32_t canvas_width;
    uint32_t canvas_height;

    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    if (!canvas || !source) return;

    canvas_width = canvas->width;
    canvas_height = canvas->height;

    cfg->source_id = source->id;
    cfg->source_type = sbs_scene_graph_source_kind_name(source->kind);
    cfg->width = canvas_width;
    cfg->height = canvas_height;
    cfg->framerate_num = canvas->fps_num;
    cfg->framerate_den = canvas->fps_den;
    cfg->pattern = source_config_string(source, "pattern");
    if (!cfg->pattern) cfg->pattern = "smpte";
    cfg->device_path = source_config_string(source, "device");
    if (!cfg->device_path) cfg->device_path = source_config_string(source, "device_path");
    cfg->uri = source_config_string(source, "uri");
    if (!cfg->uri) cfg->uri = source_config_string(source, "path");
    cfg->loop = source_config_bool(source, "loop",
                                   source->kind == SBS_SOURCE_KIND_IMAGE ||
                                   source->kind == SBS_SOURCE_KIND_URIDECODEBIN);

    if (source->kind == SBS_SOURCE_KIND_VFMCAP) {
        cfg->capture_mode = "passthrough";
        cfg->output_format = source_config_string(source, "output_format");
        if (!cfg->output_format) cfg->output_format = canvas_vfmcap_output_format(canvas);
        if (!cfg->device_path) cfg->device_path = "/dev/video_cap";
    } else if (source->kind == SBS_SOURCE_KIND_VIDEOTESTSRC) {
        uint32_t default_w = MIN(cfg->width, 1280u);
        uint32_t default_h = MIN(cfg->height, 720u);
        uint32_t default_fps = MIN(cfg->framerate_num > 0 ? cfg->framerate_num : 30u, 30u);
        cfg->width = source_config_u32(source, "width", default_w, 16, canvas_width);
        cfg->height = source_config_u32(source, "height", default_h, 16, canvas_height);
        cfg->framerate_num = source_config_u32(source, "fps", default_fps, 1, 120);
        cfg->framerate_den = 1;
        cfg->capture_mode = source_config_string(source, "color_mode");
        cfg->output_format = source_config_string(source, "output_format");
    } else if (source->kind == SBS_SOURCE_KIND_IMAGE) {
        uint32_t default_w = MIN(cfg->width, 1280u);
        uint32_t default_h = MIN(cfg->height, 720u);
        cfg->width = source_config_u32(source, "width", default_w, 16, canvas_width);
        cfg->height = source_config_u32(source, "height", default_h, 16, canvas_height);
        cfg->framerate_num = source_config_u32(source, "fps", 1u, 1, 30);
        cfg->framerate_den = 1;
    } else if (source->kind == SBS_SOURCE_KIND_TEXT) {
        uint32_t default_w = MIN(cfg->width, 1280u);
        cfg->width = source_config_u32(source, "width", default_w, 16, canvas_width);
        cfg->height = source_config_u32(source, "height", 256u, 16, canvas_height);
        cfg->framerate_num = source_config_u32(source, "fps", 1u, 1, 30);
        cfg->framerate_den = 1;
        cfg->text = source_config_string(source, "text");
        cfg->font_family = source_config_string(source, "font_family");
        cfg->font_path = source_config_string(source, "font_path");
        cfg->text_color = source_config_string(source, "text_color");
        cfg->text_align = source_config_string(source, "text_align");
        cfg->font_size = source_config_u32(source, "font_size", 72u, 4, 512);
    } else {
        cfg->capture_mode = source_config_string(source, "color_mode");
        cfg->output_format = source_config_string(source, "output_format");
    }
}
