#define _GNU_SOURCE
#define SBS_LOG_COMP "scene-graph"

#include "sbs/scene_graph.h"
#include "sbs/log.h"
#include <cjson/cJSON.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <drm/drm_fourcc.h>

static char *dup_or_null(const char *s)
{
    return s ? g_strdup(s) : NULL;
}

static double snap_quarter_rotation(double degrees);

static bool valid_graph_id(const char *id)
{
    size_t len;

    if (!id || !*id) return false;
    len = strlen(id);
    if (len > 96) return false;
    for (const char *p = id; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
            return false;
        }
    }
    return true;
}

static GHashTable *str_map_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void str_map_set(GHashTable *map, const char *key, const char *value)
{
    if (!map || !key) {
        return;
    }
    g_hash_table_replace(map, g_strdup(key), g_strdup(value ? value : ""));
}

static void audio_binding_clear(sbs_audio_binding_t *audio)
{
    if (!audio) {
        return;
    }
    g_free(audio->device);
    memset(audio, 0, sizeof(*audio));
}

static void audio_binding_copy(sbs_audio_binding_t *dst,
                               const sbs_audio_binding_t *src)
{
    if (!dst) {
        return;
    }
    audio_binding_clear(dst);
    if (!src) {
        dst->volume = 1.0;
        return;
    }
    dst->enabled = src->enabled;
    dst->device = dup_or_null(src->device);
    dst->volume = src->volume >= 0.0 ? src->volume : 1.0;
    dst->left_gain = src->left_gain >= 0.0 ? src->left_gain : 1.0;
    dst->right_gain = src->right_gain >= 0.0 ? src->right_gain : 1.0;
    dst->delay_ms = src->delay_ms;
    memcpy(dst->eq_bands, src->eq_bands, sizeof(dst->eq_bands));
    dst->mute = src->mute;
    dst->monitor = src->monitor;
}

static void filter_state_free(gpointer data)
{
    sbs_filter_state_t *filter = data;
    if (!filter) {
        return;
    }
    g_free(filter->id);
    g_free(filter->type);
    if (filter->params) {
        g_hash_table_destroy(filter->params);
    }
    g_free(filter);
}

static void scene_item_transform_clear(sbs_scene_item_transform_t *transform)
{
    if (!transform) {
        return;
    }
    g_free(transform->bounds_type);
    g_free(transform->alignment);
    memset(transform, 0, sizeof(*transform));
}

static void scene_item_transform_copy(sbs_scene_item_transform_t *dst,
                                      const sbs_scene_item_transform_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->position_x = src->position_x;
    dst->position_y = src->position_y;
    dst->width = src->width;
    dst->height = src->height;
    dst->crop_top = src->crop_top;
    dst->crop_bottom = src->crop_bottom;
    dst->crop_left = src->crop_left;
    dst->crop_right = src->crop_right;
    dst->rotation_deg = snap_quarter_rotation(src->rotation_deg);
    dst->flip_horizontal = src->flip_horizontal;
    dst->flip_vertical = src->flip_vertical;
    dst->bounds_type = dup_or_null(src->bounds_type ? src->bounds_type : "stretch");
    dst->alignment = dup_or_null(src->alignment ? src->alignment : "center");
    dst->opacity = src->opacity > 0.0 ? src->opacity : 1.0;
}

static void scene_item_state_free(gpointer data)
{
    sbs_scene_item_state_t *item = data;
    if (!item) {
        return;
    }
    g_free(item->id);
    g_free(item->source_id);
    scene_item_transform_clear(&item->transform);
    audio_binding_clear(&item->audio);
    if (item->filters) {
        g_ptr_array_free(item->filters, TRUE);
    }
    g_free(item);
}

static void scene_state_free(gpointer data)
{
    sbs_scene_state_t *scene = data;
    if (!scene) {
        return;
    }
    g_free(scene->id);
    g_free(scene->name);
    if (scene->items) {
        g_ptr_array_free(scene->items, TRUE);
    }
    if (scene->filters) {
        g_ptr_array_free(scene->filters, TRUE);
    }
    g_free(scene);
}

static void sink_state_free(gpointer data)
{
    sbs_sink_state_t *sink = data;
    if (!sink) {
        return;
    }
    g_free(sink->id);
    g_free(sink->type);
    if (sink->config) {
        g_hash_table_destroy(sink->config);
    }
    g_free(sink);
}

static void output_state_free(gpointer data)
{
    sbs_output_state_t *output = data;
    if (!output) {
        return;
    }
    g_free(output->id);
    g_free(output->name);
    g_free(output->runtime_state);
    if (output->encoder) {
        g_hash_table_destroy(output->encoder);
    }
    if (output->sinks) {
        g_ptr_array_free(output->sinks, TRUE);
    }
    g_free(output);
}

static void transition_state_free(gpointer data)
{
    sbs_transition_state_t *transition = data;
    if (!transition) {
        return;
    }
    g_free(transition->id);
    if (transition->params) {
        g_hash_table_destroy(transition->params);
    }
    g_free(transition);
}

void source_state_free(gpointer data)
{
    sbs_source_state_t *source = data;

    if (!source) {
        return;
    }
    if (source->slot_initialized) {
        sbs_frame_slot_destroy(&source->frame_slot);
    }
    g_free(source->id);
    g_free(source->name);
    if (source->config) {
        g_hash_table_destroy(source->config);
    }
    if (source->filters) {
        g_ptr_array_free(source->filters, TRUE);
    }
    audio_binding_clear(&source->audio);
    g_free(source->runtime_state);
    g_free(source->v4l2_effective_decode_mode);
    g_free(source->error_message);
    g_free(source);
}

static int item_compare_z_order(gconstpointer a, gconstpointer b)
{
    const sbs_scene_item_state_t *ia = *(const sbs_scene_item_state_t * const *)a;
    const sbs_scene_item_state_t *ib = *(const sbs_scene_item_state_t * const *)b;

    if (ia->z_order < ib->z_order) {
        return -1;
    }
    if (ia->z_order > ib->z_order) {
        return 1;
    }
    return g_strcmp0(ia->id, ib->id);
}

static void scene_sort_items(sbs_scene_state_t *scene)
{
    if (!scene || !scene->items) {
        return;
    }
    g_ptr_array_sort(scene->items, item_compare_z_order);
}

static GHashTable *str_map_copy(GHashTable *src)
{
    GHashTable *dst = str_map_new();
    GHashTableIter iter;
    gpointer key, value;
    if (!src) {
        return dst;
    }
    g_hash_table_iter_init(&iter, src);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        str_map_set(dst, key, value);
    }
    return dst;
}

static double snap_quarter_rotation(double degrees)
{
    int quarter = (int)lround(degrees / 90.0);
    quarter = ((quarter % 4) + 4) % 4;
    return (double)(quarter * 90);
}

static GHashTable *filter_params_copy(const char *type, GHashTable *src)
{
    GHashTable *dst = str_map_copy(src);
    if (g_strcmp0(type, "rotation") == 0) {
        const char *value = g_hash_table_lookup(dst, "degrees");
        double degrees = value ? g_ascii_strtod(value, NULL) : 90.0;
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%.0f", snap_quarter_rotation(degrees));
        str_map_set(dst, "degrees", buf);
    }
    return dst;
}

static sbs_filter_state_t *filter_array_find(GPtrArray *filters,
                                             const char *filter_id,
                                             guint *out_index)
{
    guint i;
    if (!filters || !filter_id) {
        return NULL;
    }
    for (i = 0; i < filters->len; i++) {
        sbs_filter_state_t *filter = g_ptr_array_index(filters, i);
        if (g_strcmp0(filter->id, filter_id) == 0) {
            if (out_index) {
                *out_index = i;
            }
            return filter;
        }
    }
    return NULL;
}

static sbs_filter_state_t *source_find_filter(sbs_source_state_t *source,
                                              const char *filter_id,
                                              guint *out_index)
{
    return source ? filter_array_find(source->filters, filter_id, out_index) : NULL;
}

static sbs_filter_state_t *scene_find_filter(sbs_scene_state_t *scene,
                                             const char *filter_id,
                                             guint *out_index)
{
    return scene ? filter_array_find(scene->filters, filter_id, out_index) : NULL;
}

static sbs_scene_item_state_t *scene_find_item(sbs_scene_state_t *scene,
                                               const char *item_id,
                                               guint *out_index)
{
    guint i;

    if (!scene || !scene->items || !item_id) {
        return NULL;
    }

    for (i = 0; i < scene->items->len; i++) {
        sbs_scene_item_state_t *item = g_ptr_array_index(scene->items, i);
        if (g_strcmp0(item->id, item_id) == 0) {
            if (out_index) {
                *out_index = i;
            }
            return item;
        }
    }

    return NULL;
}

static void graph_bump_version(sbs_scene_graph_t *graph)
{
    if (graph) {
        graph->version++;
    }
}

static void background_hex_to_rgba(const char *hex, float out[4])
{
    unsigned int r = 0, g = 0, b = 0;
    if (!hex || sscanf(hex, "#%02x%02x%02x", &r, &g, &b) != 3) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    out[0] = (float)r / 255.0f;
    out[1] = (float)g / 255.0f;
    out[2] = (float)b / 255.0f;
    out[3] = 1.0f;
}

static void hash_source_tint(const char *id, float out[4])
{
    guint hash = g_str_hash(id ? id : "source");
    out[0] = 0.2f + ((hash >> 0) & 0xff) / 255.0f * 0.6f;
    out[1] = 0.2f + ((hash >> 8) & 0xff) / 255.0f * 0.6f;
    out[2] = 0.2f + ((hash >> 16) & 0xff) / 255.0f * 0.6f;
    out[3] = 1.0f;
}

static void build_item_transform_matrix(const sbs_canvas_state_t *canvas,
                                        const sbs_scene_item_transform_t *transform,
                                        float out[16])
{
    /* Vulkan NDC: X=-1 left, X=+1 right, Y=-1 top, Y=+1 bottom (Y-down).
     * Scene coords: (0,0) = top-left, Y increases downward.
     * Map scene rect [pos_x, pos_x+w] x [pos_y, pos_y+h] to NDC. */
    float sx = 2.0f * (float)transform->width / (float)canvas->width;
    float sy = 2.0f * (float)transform->height / (float)canvas->height;
    float cx = -1.0f + 2.0f * ((float)transform->position_x + (float)transform->width * 0.5f) / (float)canvas->width;
    float cy = -1.0f + 2.0f * ((float)transform->position_y + (float)transform->height * 0.5f) / (float)canvas->height;
    float angle = (float)(transform->rotation_deg * M_PI / 180.0);
    float c = cosf(angle);
    float s = sinf(angle);
    float flip_x = transform->flip_horizontal ? -1.0f : 1.0f;
    float flip_y = transform->flip_vertical ? -1.0f : 1.0f;
    float ax = sx * flip_x * c;
    float ay = sx * flip_x * s;
    float bx = -sy * flip_y * s;
    float by = sy * flip_y * c;

    memset(out, 0, sizeof(float) * 16);
    out[0] = ax;
    out[1] = ay;
    out[4] = bx;
    out[5] = by;
    out[10] = 1.0f;
    out[12] = cx - 0.5f * ax - 0.5f * bx;
    out[13] = cy - 0.5f * ay - 0.5f * by;
    out[15] = 1.0f;
}

static void normalize_crop_pair(float *a, float *b)
{
    float sum;

    if (!a || !b)
        return;
    *a = CLAMP(*a, 0.0f, 0.98f);
    *b = CLAMP(*b, 0.0f, 0.98f);
    sum = *a + *b;
    if (sum > 0.98f) {
        float scale = 0.98f / sum;
        *a *= scale;
        *b *= scale;
    }
}

static float filter_float_param(const sbs_filter_state_t *filter,
                                const char *key,
                                float fallback);
static void filter_rgb_param(const sbs_filter_state_t *filter,
                             float fallback_r,
                             float fallback_g,
                             float fallback_b,
                             float *r,
                             float *g,
                             float *b);

static void apply_filter_to_comp_item(const sbs_filter_state_t *filter,
                                      sbs_comp_scene_item_t *out)
{
    const char *value;
    float gray;

    if (!filter || !filter->enabled || !out) {
        return;
    }

    if (g_strcmp0(filter->type, "grayscale") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_GRAYSCALE;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[0] = value ? (float)g_ascii_strtod(value, NULL) : 1.0f;
        gray = out->tint[0] * 0.299f + out->tint[1] * 0.587f + out->tint[2] * 0.114f;
        out->tint[0] = out->tint[0] * (1.0f - out->filter_params[0]) + gray * out->filter_params[0];
        out->tint[1] = out->tint[1] * (1.0f - out->filter_params[0]) + gray * out->filter_params[0];
        out->tint[2] = out->tint[2] * (1.0f - out->filter_params[0]) + gray * out->filter_params[0];
    } else if (g_strcmp0(filter->type, "brightness") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_BRIGHTNESS;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[1] = value ? (float)g_ascii_strtod(value, NULL) : 0.15f;
        out->tint[0] = CLAMP(out->tint[0] + out->filter_params[1], 0.0f, 1.0f);
        out->tint[1] = CLAMP(out->tint[1] + out->filter_params[1], 0.0f, 1.0f);
        out->tint[2] = CLAMP(out->tint[2] + out->filter_params[1], 0.0f, 1.0f);
    } else if (g_strcmp0(filter->type, "contrast") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_CONTRAST;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[2] = value ? (float)g_ascii_strtod(value, NULL) : 1.15f;
        out->tint[0] = CLAMP(((out->tint[0] - 0.5f) * out->filter_params[2]) + 0.5f, 0.0f, 1.0f);
        out->tint[1] = CLAMP(((out->tint[1] - 0.5f) * out->filter_params[2]) + 0.5f, 0.0f, 1.0f);
        out->tint[2] = CLAMP(((out->tint[2] - 0.5f) * out->filter_params[2]) + 0.5f, 0.0f, 1.0f);
    } else if (g_strcmp0(filter->type, "blur") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_BLUR;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[3] = value ? (float)g_ascii_strtod(value, NULL) : 0.35f;
    } else if (g_strcmp0(filter->type, "sharpen") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_SHARPEN;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[4] = value ? (float)g_ascii_strtod(value, NULL) : 0.4f;
    } else if (g_strcmp0(filter->type, "color_correction") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_COLOR_CORRECTION;
        out->filter_params[0] = CLAMP(filter_float_param(filter, "saturation", 1.0f), 0.0f, 3.0f);
        out->filter_params[1] = CLAMP(filter_float_param(filter, "brightness", 0.0f), -1.0f, 1.0f);
        out->filter_params[2] = CLAMP(filter_float_param(filter, "contrast", 1.0f), 0.0f, 4.0f);
        out->filter_params[3] = CLAMP(filter_float_param(filter, "gamma", 1.0f), 0.1f, 4.0f);
        out->filter_params[7] = CLAMP(filter_float_param(filter, "hue", 0.0f), -180.0f, 180.0f);
        gray = out->tint[0] * 0.299f + out->tint[1] * 0.587f + out->tint[2] * 0.114f;
        out->tint[0] = gray + (out->tint[0] - gray) * out->filter_params[0];
        out->tint[1] = gray + (out->tint[1] - gray) * out->filter_params[0];
        out->tint[2] = gray + (out->tint[2] - gray) * out->filter_params[0];
        out->tint[0] = CLAMP(((out->tint[0] - 0.5f) * out->filter_params[2]) + 0.5f + out->filter_params[1], 0.0f, 1.0f);
        out->tint[1] = CLAMP(((out->tint[1] - 0.5f) * out->filter_params[2]) + 0.5f + out->filter_params[1], 0.0f, 1.0f);
        out->tint[2] = CLAMP(((out->tint[2] - 0.5f) * out->filter_params[2]) + 0.5f + out->filter_params[1], 0.0f, 1.0f);
    } else if (g_strcmp0(filter->type, "luma_key") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_LUMA_KEY;
        out->filter_params[3] = CLAMP(filter_float_param(filter, "min", 0.0f), 0.0f, 1.0f);
        out->filter_params[6] = CLAMP(filter_float_param(filter, "max", 1.0f), 0.0f, 1.0f);
        out->filter_params[7] = CLAMP(filter_float_param(filter, "smoothness", 0.08f), 0.001f, 1.0f);
    } else if (g_strcmp0(filter->type, "chroma_key") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_CHROMA_KEY;
        filter_rgb_param(filter, 0.0f, 1.0f, 0.0f,
                         &out->filter_params[0],
                         &out->filter_params[1],
                         &out->filter_params[2]);
        out->filter_params[3] = CLAMP(filter_float_param(filter, "similarity", 0.25f), 0.0f, 1.0f);
        out->filter_params[6] = CLAMP(filter_float_param(filter, "smoothness", 0.08f), 0.001f, 1.0f);
        out->filter_params[7] = CLAMP(filter_float_param(filter, "spill", 0.0f), 0.0f, 1.0f);
    } else if (g_strcmp0(filter->type, "hdr_to_sdr_lut") == 0) {
        const char *path;
        out->filter_flags |= SBS_COMP_FILTER_HDR_TO_SDR_LUT;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[5] = value ? (float)g_ascii_strtod(value, NULL) : 1.0f;
        value = filter->params ? g_hash_table_lookup(filter->params, "saturation") : NULL;
        out->hdr_to_sdr_saturation = value ? CLAMP((float)g_ascii_strtod(value, NULL), 0.0f, 3.0f) : 1.42f;
        value = filter->params ? g_hash_table_lookup(filter->params, "brightness") : NULL;
        out->hdr_to_sdr_brightness = value ? CLAMP((float)g_ascii_strtod(value, NULL), -0.3f, 0.3f) : -0.02f;
        value = filter->params ? g_hash_table_lookup(filter->params, "hue") : NULL;
        out->hdr_to_sdr_hue_deg = value ? CLAMP((float)g_ascii_strtod(value, NULL), -180.0f, 180.0f) : 0.0f;
        path = filter->params ? g_hash_table_lookup(filter->params, "path") : NULL;
        if (!path || !*path)
            path = filter->params ? g_hash_table_lookup(filter->params, "file") : NULL;
        if (path && *path)
            g_strlcpy(out->lut_path, path, sizeof(out->lut_path));
    } else if (g_strcmp0(filter->type, "sdr_to_hdr") == 0) {
        out->filter_flags |= SBS_COMP_FILTER_SDR_TO_HDR;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[5] = value ? (float)g_ascii_strtod(value, NULL) : 1.0f;
        value = filter->params ? g_hash_table_lookup(filter->params, "saturation") : NULL;
        out->hdr_to_sdr_saturation = value ? CLAMP((float)g_ascii_strtod(value, NULL), 0.5f, 2.5f) : 1.12f;
        value = filter->params ? g_hash_table_lookup(filter->params, "brightness") : NULL;
        out->hdr_to_sdr_brightness = value ? CLAMP((float)g_ascii_strtod(value, NULL), -0.3f, 0.3f) : 0.0f;
        value = filter->params ? g_hash_table_lookup(filter->params, "hue") : NULL;
        out->hdr_to_sdr_hue_deg = value ? CLAMP((float)g_ascii_strtod(value, NULL), -180.0f, 180.0f) : 0.0f;
    } else if (g_strcmp0(filter->type, "lut") == 0) {
        const char *path;
        value = filter->params ? g_hash_table_lookup(filter->params, "amount") : NULL;
        out->filter_params[5] = value ? (float)g_ascii_strtod(value, NULL) : 1.0f;
        path = filter->params ? g_hash_table_lookup(filter->params, "path") : NULL;
        if (!path || !*path)
            path = filter->params ? g_hash_table_lookup(filter->params, "file") : NULL;
        if (path && *path) {
            out->filter_flags |= SBS_COMP_FILTER_LUT;
            g_strlcpy(out->lut_path, path, sizeof(out->lut_path));
        }
    } else if (g_strcmp0(filter->type, "crop") == 0) {
        out->crop[0] += CLAMP(filter_float_param(filter, "left", 0.0f), 0.0f, 0.98f);
        out->crop[1] += CLAMP(filter_float_param(filter, "top", 0.0f), 0.0f, 0.98f);
        out->crop[2] += CLAMP(filter_float_param(filter, "right", 0.0f), 0.0f, 0.98f);
        out->crop[3] += CLAMP(filter_float_param(filter, "bottom", 0.0f), 0.0f, 0.98f);
        normalize_crop_pair(&out->crop[0], &out->crop[2]);
        normalize_crop_pair(&out->crop[1], &out->crop[3]);
    } else if (g_strcmp0(filter->type, "mirror") == 0) {
        out->flip_horizontal = !out->flip_horizontal;
    } else if (g_strcmp0(filter->type, "flip") == 0) {
        out->flip_vertical = !out->flip_vertical;
    } else if (g_strcmp0(filter->type, "rotation") == 0) {
        float deg = filter_float_param(filter, "degrees", 90.0f);
        out->rotation_deg += (float)snap_quarter_rotation(deg);
    }
}

static float filter_float_param(const sbs_filter_state_t *filter,
                                const char *key,
                                float fallback)
{
    const char *value = filter && filter->params ? g_hash_table_lookup(filter->params, key) : NULL;
    return value ? (float)g_ascii_strtod(value, NULL) : fallback;
}

static void filter_rgb_param(const sbs_filter_state_t *filter,
                             float fallback_r,
                             float fallback_g,
                             float fallback_b,
                             float *r,
                             float *g,
                             float *b)
{
    const char *value = filter && filter->params ? g_hash_table_lookup(filter->params, "color") : NULL;
    unsigned int rv, gv, bv;

    if (value && value[0] == '#')
        value++;
    if (value && strlen(value) == 6 && sscanf(value, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
        *r = CLAMP((float)rv / 255.0f, 0.0f, 1.0f);
        *g = CLAMP((float)gv / 255.0f, 0.0f, 1.0f);
        *b = CLAMP((float)bv / 255.0f, 0.0f, 1.0f);
        return;
    }

    *r = CLAMP(filter_float_param(filter, "red", fallback_r), 0.0f, 1.0f);
    *g = CLAMP(filter_float_param(filter, "green", fallback_g), 0.0f, 1.0f);
    *b = CLAMP(filter_float_param(filter, "blue", fallback_b), 0.0f, 1.0f);
}

static void apply_filters_to_comp_item(GPtrArray *filters, sbs_comp_scene_item_t *out)
{
    guint i;
    for (i = 0; filters && i < filters->len; i++) {
        apply_filter_to_comp_item(g_ptr_array_index(filters, i), out);
    }
}

static void apply_filters_to_background(GPtrArray *filters, float rgba[4])
{
    sbs_comp_scene_item_t tmp;
    if (!filters || filters->len == 0 || !rgba) {
        return;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.tint[0] = rgba[0];
    tmp.tint[1] = rgba[1];
    tmp.tint[2] = rgba[2];
    tmp.tint[3] = rgba[3];
    apply_filters_to_comp_item(filters, &tmp);
    rgba[0] = tmp.tint[0];
    rgba[1] = tmp.tint[1];
    rgba[2] = tmp.tint[2];
    rgba[3] = tmp.tint[3];
}

static void fill_comp_item(const sbs_canvas_state_t *canvas,
                           const sbs_scene_item_state_t *item,
                           sbs_comp_scene_item_t *out,
                           GHashTable *sources,
                           GPtrArray *scene_filters)
{
    sbs_scene_item_transform_t render_transform;
    sbs_source_state_t *source = NULL;
    GPtrArray *filters = NULL;
    memset(out, 0, sizeof(*out));
    g_strlcpy(out->source_id, item->source_id ? item->source_id : "source", sizeof(out->source_id));
    out->visible = item->visible;

    /* Link to source's frame slot so the compositor thread can read live frames */
    out->frame_slot = NULL;
    out->frame_width = 0;
    out->frame_height = 0;
    out->color_depth = 8;
    out->hdr = false;
    if (item->source_id && sources) {
        source = g_hash_table_lookup(sources, item->source_id);
        if (source && source->slot_initialized) {
            out->frame_slot = &source->frame_slot;
            out->frame_width = source->frame_width;
            out->frame_height = source->frame_height;
            out->color_depth = source->color_depth ? source->color_depth : 8;
            out->hdr = source->hdr;
            out->drm_format = source->drm_format;
            out->drm_modifier = source->drm_modifier;
            out->plane_offset[0] = source->plane_offset[0];
            out->plane_offset[1] = source->plane_offset[1];
            out->plane_stride[0] = source->plane_stride[0];
            out->plane_stride[1] = source->plane_stride[1];
        }
        if (source) {
            filters = source->filters;
        }
    }
    out->z_order = item->z_order;
    out->crop[0] = item->transform.width > 0 ? (float)item->transform.crop_left / (float)item->transform.width : 0.0f;
    out->crop[1] = item->transform.height > 0 ? (float)item->transform.crop_top / (float)item->transform.height : 0.0f;
    out->crop[2] = item->transform.width > 0 ? (float)item->transform.crop_right / (float)item->transform.width : 0.0f;
    out->crop[3] = item->transform.height > 0 ? (float)item->transform.crop_bottom / (float)item->transform.height : 0.0f;
    out->opacity = (float)item->transform.opacity;
    out->render_x = item->transform.position_x;
    out->render_y = item->transform.position_y;
    out->render_width = item->transform.width;
    out->render_height = item->transform.height;
    out->flip_horizontal = item->transform.flip_horizontal;
    out->flip_vertical = item->transform.flip_vertical;
    out->rotation_deg = (float)item->transform.rotation_deg;
    hash_source_tint(item->source_id, out->tint);
    apply_filters_to_comp_item(filters, out);
    apply_filters_to_comp_item(item->filters, out);
    apply_filters_to_comp_item(scene_filters, out);
    out->rotation_deg = (float)snap_quarter_rotation(out->rotation_deg);
    render_transform = item->transform;
    render_transform.flip_horizontal = out->flip_horizontal;
    render_transform.flip_vertical = out->flip_vertical;
    render_transform.rotation_deg = out->rotation_deg;
    build_item_transform_matrix(canvas, &render_transform, out->transform);
}

const char *sbs_scene_graph_source_kind_name(sbs_source_kind_t kind)
{
    switch (kind) {
    case SBS_SOURCE_KIND_V4L2SRC: return "v4l2src";
    case SBS_SOURCE_KIND_URIDECODEBIN: return "uridecodebin";
    case SBS_SOURCE_KIND_VIDEOTESTSRC: return "videotestsrc";
    case SBS_SOURCE_KIND_IMAGE: return "image";
    case SBS_SOURCE_KIND_TEXT: return "text";
    case SBS_SOURCE_KIND_VFMCAP: return "vfmcap";
    case SBS_SOURCE_KIND_ALSA_AUDIO: return "alsa_audio";
    default: return "videotestsrc";
    }
}

bool sbs_scene_graph_parse_source_kind(const char *type, sbs_source_kind_t *out_kind)
{
    if (!out_kind) return false;
    if (!type || strcmp(type, "videotestsrc") == 0) *out_kind = SBS_SOURCE_KIND_VIDEOTESTSRC;
    else if (strcmp(type, "v4l2src") == 0) *out_kind = SBS_SOURCE_KIND_V4L2SRC;
    else if (strcmp(type, "uridecodebin") == 0) *out_kind = SBS_SOURCE_KIND_URIDECODEBIN;
    else if (strcmp(type, "image") == 0) *out_kind = SBS_SOURCE_KIND_IMAGE;
    else if (strcmp(type, "text") == 0) *out_kind = SBS_SOURCE_KIND_TEXT;
    else if (strcmp(type, "vfmcap") == 0) *out_kind = SBS_SOURCE_KIND_VFMCAP;
    else if (strcmp(type, "alsa_audio") == 0 || strcmp(type, "audio") == 0) *out_kind = SBS_SOURCE_KIND_ALSA_AUDIO;
    else return false;
    return true;
}

static cJSON *serialize_audio_binding(const sbs_audio_binding_t *binding)
{
    cJSON *audio = cJSON_CreateObject();
    cJSON *eq = cJSON_CreateArray();

    cJSON_AddBoolToObject(audio, "enabled", binding->enabled);
    if (binding->device) {
        cJSON_AddStringToObject(audio, "device", binding->device);
    } else {
        cJSON_AddNullToObject(audio, "device");
    }
    cJSON_AddNumberToObject(audio, "volume", binding->volume >= 0.0 ? binding->volume : 1.0);
    cJSON_AddNumberToObject(audio, "left_gain", binding->left_gain >= 0.0 ? binding->left_gain : 1.0);
    cJSON_AddNumberToObject(audio, "right_gain", binding->right_gain >= 0.0 ? binding->right_gain : 1.0);
    cJSON_AddNumberToObject(audio, "delay_ms", binding->delay_ms);
    for (uint32_t i = 0; i < G_N_ELEMENTS(binding->eq_bands); i++) {
        cJSON_AddItemToArray(eq, cJSON_CreateNumber(binding->eq_bands[i]));
    }
    cJSON_AddItemToObject(audio, "eq_bands", eq);
    cJSON_AddBoolToObject(audio, "mute", binding->mute);
    cJSON_AddBoolToObject(audio, "monitor", binding->monitor);
    return audio;
}

const char *sbs_scene_graph_transition_kind_name(sbs_transition_kind_t kind)
{
    switch (kind) {
    case SBS_TRANSITION_KIND_CUT: return "cut";
    case SBS_TRANSITION_KIND_FADE: return "fade";
    case SBS_TRANSITION_KIND_SLIDE: return "slide";
    default: return "cut";
    }
}

static cJSON *str_map_to_json(GHashTable *map)
{
    cJSON *obj = cJSON_CreateObject();
    if (!map) {
        return obj;
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddStringToObject(obj, (const char *)key, (const char *)value);
    }

    return obj;
}

static const char *str_map_get(GHashTable *map, const char *key)
{
    if (!map || !key)
        return NULL;
    return g_hash_table_lookup(map, key);
}

static uint32_t str_map_get_u32(GHashTable *map, const char *key, uint32_t fallback)
{
    const char *value = str_map_get(map, key);
    char *end = NULL;
    guint64 parsed;

    if (!value || !value[0])
        return fallback;
    parsed = g_ascii_strtoull(value, &end, 10);
    if (end == value || parsed > G_MAXUINT32)
        return fallback;
    return (uint32_t)parsed;
}

static bool v4l2_fourcc_is_compressed(const char *fourcc)
{
    return g_strcmp0(fourcc, "MJPG") == 0 ||
           g_strcmp0(fourcc, "JPEG") == 0 ||
           g_strcmp0(fourcc, "H264") == 0 ||
           g_strcmp0(fourcc, "AVC1") == 0 ||
           g_strcmp0(fourcc, "H265") == 0 ||
           g_strcmp0(fourcc, "HEVC") == 0;
}

static bool v4l2_fourcc_is_direct_importable(const char *fourcc)
{
    return g_strcmp0(fourcc, "NV12") == 0 ||
           g_strcmp0(fourcc, "NV21") == 0 ||
           g_strcmp0(fourcc, "P010") == 0 ||
           g_strcmp0(fourcc, "RGBA") == 0;
}

static bool gst_element_available(const char *name)
{
    gchar *inspect_path = NULL;
    gint status = 0;
    gboolean ok;

    if (!name || !name[0])
        return false;

    inspect_path = g_find_program_in_path("gst-inspect-1.0");
    if (!inspect_path && g_file_test("/usr/bin/gst-inspect-1.0", G_FILE_TEST_IS_EXECUTABLE))
        inspect_path = g_strdup("/usr/bin/gst-inspect-1.0");
    if (!inspect_path)
        return false;

    gchar *argv[] = { inspect_path, (gchar *)name, NULL };
    ok = g_spawn_sync(NULL, argv, NULL,
                      G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, NULL);
    g_free(inspect_path);
    return ok && status == 0;
}

static bool v4l2_hardware_decode_available(const char *fourcc)
{
    static int have_aml_jpeg = -1;
    static int have_aml_h264 = -1;

    if (g_strcmp0(fourcc, "MJPG") == 0 || g_strcmp0(fourcc, "JPEG") == 0) {
        if (have_aml_jpeg < 0)
            have_aml_jpeg = gst_element_available("amlv4l2jpegdec") ? 1 : 0;
        return have_aml_jpeg == 1;
    }
    if (g_strcmp0(fourcc, "H264") == 0 || g_strcmp0(fourcc, "AVC1") == 0) {
        if (have_aml_h264 < 0)
            have_aml_h264 = gst_element_available("amlv4l2h264dec") ? 1 : 0;
        return have_aml_h264 == 1;
    }
    return false;
}

static cJSON *serialize_v4l2_status(const sbs_source_state_t *source)
{
    GHashTable *config = source ? source->config : NULL;
    const char *device_id = str_map_get(config, "device_id");
    const char *device_path = str_map_get(config, "device");
    const char *fourcc = str_map_get(config, "format");
    const char *framerate = str_map_get(config, "framerate");
    const char *decode_mode = str_map_get(config, "decode_mode");
    const char *effective_decode_mode = source ? source->v4l2_effective_decode_mode : NULL;
    bool compressed;
    bool direct_importable;
    cJSON *obj = cJSON_CreateObject();

    if (!device_path)
        device_path = str_map_get(config, "device_path");
    if (!fourcc)
        fourcc = str_map_get(config, "fourcc");
    if (!framerate)
        framerate = str_map_get(config, "fps");
    if (!decode_mode)
        decode_mode = "auto";
    if (!effective_decode_mode)
        effective_decode_mode = decode_mode;

    compressed = v4l2_fourcc_is_compressed(fourcc);
    direct_importable = !compressed && v4l2_fourcc_is_direct_importable(fourcc);

    if (device_id)
        cJSON_AddStringToObject(obj, "device_id", device_id);
    else
        cJSON_AddNullToObject(obj, "device_id");
    cJSON_AddStringToObject(obj, "device_path", device_path ? device_path : "/dev/video0");
    if (fourcc)
        cJSON_AddStringToObject(obj, "fourcc", fourcc);
    else
        cJSON_AddNullToObject(obj, "fourcc");
    cJSON_AddNumberToObject(obj, "width",
                            str_map_get_u32(config, "width", source ? source->frame_width : 0));
    cJSON_AddNumberToObject(obj, "height",
                            str_map_get_u32(config, "height", source ? source->frame_height : 0));
    if (framerate)
        cJSON_AddStringToObject(obj, "framerate", framerate);
    else
        cJSON_AddNullToObject(obj, "framerate");
    cJSON_AddStringToObject(obj, "requested_decode_mode", decode_mode);
    cJSON_AddStringToObject(obj, "effective_decode_mode", effective_decode_mode);

    if (!source || !source->running) {
        cJSON_AddStringToObject(obj, "active_decode_path", "inactive");
        cJSON_AddNullToObject(obj, "zero_copy_active");
        cJSON_AddStringToObject(obj, "zero_copy_state", "inactive");
    } else if (compressed) {
        const char *active_decode = "software";
        if (g_strcmp0(effective_decode_mode, "hardware") == 0 ||
            (g_strcmp0(effective_decode_mode, "auto") == 0 && v4l2_hardware_decode_available(fourcc))) {
            active_decode = "hardware";
        }
        cJSON_AddStringToObject(obj, "active_decode_path", active_decode);
        cJSON_AddBoolToObject(obj, "zero_copy_active", false);
        cJSON_AddStringToObject(obj, "zero_copy_state", "decode");
    } else if (direct_importable) {
        cJSON_AddStringToObject(obj, "active_decode_path", "none");
        cJSON_AddNullToObject(obj, "zero_copy_active");
        cJSON_AddStringToObject(obj, "zero_copy_state", "expected");
    } else {
        cJSON_AddStringToObject(obj, "active_decode_path", "none");
        cJSON_AddBoolToObject(obj, "zero_copy_active", false);
        cJSON_AddStringToObject(obj, "zero_copy_state", "fallback");
    }

    return obj;
}

static cJSON *output_encoder_to_public_json(GHashTable *map)
{
    cJSON *obj = cJSON_CreateObject();
    if (!map) {
        return obj;
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_strcmp0((const char *)key, "rtmp_passcode") == 0) {
            const char *secret = value ? (const char *)value : "";
            bool secret_set = secret[0] != '\0';
            double secret_len = secret_set ? (double)strlen(secret) : 0.0;
            cJSON_AddBoolToObject(obj, "rtmp_passcode_set", secret_set);
            cJSON_AddBoolToObject(obj, "rtmp_stream_key_set", secret_set);
            cJSON_AddNumberToObject(obj, "rtmp_passcode_length", secret_len);
            cJSON_AddNumberToObject(obj, "rtmp_stream_key_length", secret_len);
            continue;
        }
        if (g_strcmp0((const char *)key, "remote_password") == 0) {
            cJSON_AddBoolToObject(obj, "remote_password_set",
                                  value && ((const char *)value)[0] != '\0');
            continue;
        }
        cJSON_AddStringToObject(obj, (const char *)key, (const char *)value);
    }

    return obj;
}

static cJSON *serialize_filter(const sbs_filter_state_t *filter)
{
    cJSON *filter_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(filter_obj, "id", filter->id);
    cJSON_AddStringToObject(filter_obj, "type", filter->type);
    cJSON_AddBoolToObject(filter_obj, "enabled", filter->enabled);
    cJSON_AddItemToObject(filter_obj, "params", str_map_to_json(filter->params));
    return filter_obj;
}

static cJSON *serialize_filters(GPtrArray *filters)
{
    cJSON *array = cJSON_CreateArray();
    guint i;
    if (!filters) {
        return array;
    }
    for (i = 0; i < filters->len; i++) {
        cJSON_AddItemToArray(array, serialize_filter(g_ptr_array_index(filters, i)));
    }
    return array;
}

static cJSON *serialize_transform(const sbs_scene_item_transform_t *transform)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "position_x", transform->position_x);
    cJSON_AddNumberToObject(obj, "position_y", transform->position_y);
    cJSON_AddNumberToObject(obj, "width", transform->width);
    cJSON_AddNumberToObject(obj, "height", transform->height);
    cJSON_AddNumberToObject(obj, "crop_top", transform->crop_top);
    cJSON_AddNumberToObject(obj, "crop_bottom", transform->crop_bottom);
    cJSON_AddNumberToObject(obj, "crop_left", transform->crop_left);
    cJSON_AddNumberToObject(obj, "crop_right", transform->crop_right);
    cJSON_AddNumberToObject(obj, "rotation_deg", transform->rotation_deg);
    cJSON_AddBoolToObject(obj, "flip_horizontal", transform->flip_horizontal);
    cJSON_AddBoolToObject(obj, "flip_vertical", transform->flip_vertical);
    cJSON_AddStringToObject(obj, "bounds_type", transform->bounds_type ? transform->bounds_type : "stretch");
    cJSON_AddStringToObject(obj, "alignment", transform->alignment ? transform->alignment : "center");
    cJSON_AddNumberToObject(obj, "opacity", transform->opacity);
    return obj;
}

static cJSON *serialize_item(const sbs_scene_item_state_t *item)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", item->id);
    cJSON_AddStringToObject(obj, "source_id", item->source_id);
    cJSON_AddBoolToObject(obj, "visible", item->visible);
    cJSON_AddBoolToObject(obj, "locked", item->locked);
    cJSON_AddNumberToObject(obj, "z_order", item->z_order);
    cJSON_AddItemToObject(obj, "transform", serialize_transform(&item->transform));
    cJSON_AddItemToObject(obj, "audio", serialize_audio_binding(&item->audio));
    cJSON_AddItemToObject(obj, "filters", serialize_filters(item->filters));
    return obj;
}

cJSON *sbs_scene_graph_serialize_source(const sbs_source_state_t *source)
{
    cJSON *obj = cJSON_CreateObject();
    sbs_frame_slot_stats_t frame_stats;

    if (!source) {
        return obj;
    }

    cJSON_AddStringToObject(obj, "id", source->id);
    cJSON_AddStringToObject(obj, "name", source->name ? source->name : source->id);
    cJSON_AddStringToObject(obj, "type", sbs_scene_graph_source_kind_name(source->kind));
    cJSON_AddBoolToObject(obj, "enabled", source->enabled);
    cJSON_AddBoolToObject(obj, "keep_alive", source->keep_alive);
    cJSON_AddStringToObject(obj, "state", source->runtime_state ? source->runtime_state : (source->running ? "running" : "created"));
    cJSON_AddBoolToObject(obj, "muted", source->muted);
    cJSON_AddItemToObject(obj, "config", str_map_to_json(source->config));
    if (source->kind == SBS_SOURCE_KIND_V4L2SRC)
        cJSON_AddItemToObject(obj, "v4l2_status", serialize_v4l2_status(source));
    cJSON_AddItemToObject(obj, "filters", serialize_filters(source->filters));
    sbs_frame_slot_get_stats((sbs_frame_slot_t *)&source->frame_slot, &frame_stats);
    cJSON *frame_queue = cJSON_CreateObject();
    cJSON_AddNumberToObject(frame_queue, "produced", (double)frame_stats.produced);
    cJSON_AddNumberToObject(frame_queue, "dropped", (double)frame_stats.dropped);
    cJSON_AddNumberToObject(frame_queue, "consumed", (double)frame_stats.consumed);
    cJSON_AddNumberToObject(frame_queue, "held", frame_stats.held);
    cJSON_AddItemToObject(obj, "frame_queue", frame_queue);

    cJSON_AddItemToObject(obj, "audio", serialize_audio_binding(&source->audio));

    if (source->error_message) {
        cJSON_AddStringToObject(obj, "error_message", source->error_message);
    }

    return obj;
}

cJSON *sbs_scene_graph_serialize_scene(const sbs_scene_state_t *scene)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    guint i;

    if (!scene) {
        cJSON_AddItemToObject(obj, "items", items);
        return obj;
    }

    cJSON_AddStringToObject(obj, "id", scene->id);
    cJSON_AddStringToObject(obj, "name", scene->name ? scene->name : scene->id);
    cJSON_AddItemToObject(obj, "filters", serialize_filters(scene->filters));
    for (i = 0; i < scene->items->len; i++) {
        cJSON_AddItemToArray(items, serialize_item(g_ptr_array_index(scene->items, i)));
    }
    cJSON_AddItemToObject(obj, "items", items);
    return obj;
}

static cJSON *serialize_output_internal(const sbs_output_state_t *output, bool public_view)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *sinks = cJSON_CreateArray();
    guint i;

    if (!output) {
        cJSON_AddItemToObject(obj, "sinks", sinks);
        return obj;
    }

    cJSON_AddStringToObject(obj, "id", output->id);
    cJSON_AddStringToObject(obj, "name", output->name ? output->name : output->id);
    cJSON_AddBoolToObject(obj, "enabled", output->enabled);
    cJSON_AddBoolToObject(obj, "autostart", output->autostart);
    cJSON_AddStringToObject(obj, "state", output->runtime_state ? output->runtime_state : (output->running ? "running" : "created"));
    cJSON_AddItemToObject(obj, "encoder", public_view
        ? output_encoder_to_public_json(output->encoder)
        : str_map_to_json(output->encoder));

    for (i = 0; i < output->sinks->len; i++) {
        sbs_sink_state_t *sink = g_ptr_array_index(output->sinks, i);
        cJSON *sink_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(sink_obj, "id", sink->id);
        cJSON_AddStringToObject(sink_obj, "type", sink->type);
        cJSON_AddBoolToObject(sink_obj, "enabled", sink->enabled);
        cJSON_AddItemToObject(sink_obj, "config", str_map_to_json(sink->config));
        cJSON_AddItemToArray(sinks, sink_obj);
    }
    cJSON_AddItemToObject(obj, "sinks", sinks);
    return obj;
}

cJSON *sbs_scene_graph_serialize_output(const sbs_output_state_t *output)
{
    return serialize_output_internal(output, false);
}

cJSON *sbs_scene_graph_serialize_output_public(const sbs_output_state_t *output)
{
    return serialize_output_internal(output, true);
}

cJSON *sbs_scene_graph_serialize_transition(const sbs_transition_state_t *transition)
{
    cJSON *obj = cJSON_CreateObject();
    if (!transition) {
        return obj;
    }
    cJSON_AddStringToObject(obj, "id", transition->id);
    cJSON_AddStringToObject(obj, "type", sbs_scene_graph_transition_kind_name(transition->kind));
    cJSON_AddNumberToObject(obj, "duration_ms", transition->duration_ms);
    cJSON_AddItemToObject(obj, "params", str_map_to_json(transition->params));
    return obj;
}

static cJSON *serialize_full_state_internal(const sbs_scene_graph_t *graph, bool public_view)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *canvas = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *scenes = cJSON_CreateObject();
    cJSON *outputs = cJSON_CreateObject();
    cJSON *transitions = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    GHashTableIter iter;
    gpointer key, value;

    cJSON_AddNumberToObject(canvas, "width", graph->canvas.width);
    cJSON_AddNumberToObject(canvas, "height", graph->canvas.height);
    cJSON_AddNumberToObject(canvas, "fps_num", graph->canvas.fps_num);
    cJSON_AddNumberToObject(canvas, "fps_den", graph->canvas.fps_den);
    cJSON_AddStringToObject(canvas, "pixel_format",
                            sbs_pixel_format_name(graph->canvas.pixel_format));
    cJSON_AddStringToObject(canvas, "colorimetry",
                            sbs_colorimetry_name(graph->canvas.colorimetry));
    cJSON_AddStringToObject(canvas, "color_mode",
                            sbs_legacy_color_mode_for_pixel_format(graph->canvas.pixel_format));
    cJSON_AddStringToObject(canvas, "background_color", graph->canvas.background_color);
    cJSON_AddItemToObject(obj, "canvas", canvas);

    g_hash_table_iter_init(&iter, graph->sources);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(sources, (const char *)key,
                              sbs_scene_graph_serialize_source(value));
    }
    cJSON_AddItemToObject(obj, "sources", sources);

    g_hash_table_iter_init(&iter, graph->scenes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(scenes, (const char *)key,
                              sbs_scene_graph_serialize_scene(value));
    }
    cJSON_AddItemToObject(obj, "scenes", scenes);

    g_hash_table_iter_init(&iter, graph->outputs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(outputs, (const char *)key,
                              public_view
                                  ? sbs_scene_graph_serialize_output_public(value)
                                  : sbs_scene_graph_serialize_output(value));
    }
    cJSON_AddItemToObject(obj, "output_groups", outputs);

    g_hash_table_iter_init(&iter, graph->transitions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(transitions, (const char *)key,
                              sbs_scene_graph_serialize_transition(value));
    }
    cJSON_AddItemToObject(obj, "transitions", transitions);

    if (graph->active_scene_id) {
        cJSON_AddStringToObject(state, "active_scene_id", graph->active_scene_id);
    } else {
        cJSON_AddNullToObject(state, "active_scene_id");
    }
    if (graph->preview_scene_id) {
        cJSON_AddStringToObject(state, "preview_scene_id", graph->preview_scene_id);
    } else {
        cJSON_AddNullToObject(state, "preview_scene_id");
    }
    cJSON_AddBoolToObject(state, "transition_active", graph->transition_runtime.active);
    cJSON_AddNumberToObject(state, "transition_progress", graph->transition_runtime.progress);
    if (graph->transition_runtime.transition_id) {
        cJSON_AddStringToObject(state, "transition_id", graph->transition_runtime.transition_id);
    } else {
        cJSON_AddNullToObject(state, "transition_id");
    }
    cJSON_AddItemToObject(obj, "state", state);

    return obj;
}

cJSON *sbs_scene_graph_serialize_full_state(const sbs_scene_graph_t *graph)
{
    return serialize_full_state_internal(graph, false);
}

cJSON *sbs_scene_graph_serialize_full_state_public(const sbs_scene_graph_t *graph)
{
    return serialize_full_state_internal(graph, true);
}

int sbs_scene_graph_build_compositor_state(const sbs_scene_graph_t *graph,
                                           sbs_comp_scene_state_t *state)
{
    const sbs_scene_state_t *scene;
    guint i;

    if (!graph || !state) {
        return SBS_ERR_INVAL;
    }

    sbs_comp_scene_state_init(state);
    background_hex_to_rgba(graph->canvas.background_color, state->background_rgba);

    scene = sbs_scene_graph_get_scene(graph, graph->active_scene_id);
    if (scene) {
        apply_filters_to_background(scene->filters, state->background_rgba);
        for (i = 0; i < scene->items->len && state->active_item_count < SBS_COMP_SCENE_MAX_ITEMS; i++) {
            sbs_scene_item_state_t *item = g_ptr_array_index(scene->items, i);
            fill_comp_item(&graph->canvas, item, &state->active_items[state->active_item_count++],
                           graph->sources, scene->filters);
        }
    }

    if (state->active_item_count > 0) {
        const sbs_comp_scene_item_t *item = &state->active_items[0];
        LOG_I("comp-state scene=%s item=%s flags=0x%x tint=(%.3f,%.3f,%.3f)",
              graph->active_scene_id ? graph->active_scene_id : "none",
              item->source_id,
              item->filter_flags,
              item->tint[0], item->tint[1], item->tint[2]);
    }

    state->transition_active = graph->transition_runtime.active;
    state->transition_progress = (float)graph->transition_runtime.progress;
    state->transition_start_time_us = graph->transition_runtime.start_time_us;
    state->transition_duration_ms = graph->transition_runtime.duration_ms;
    if (graph->transition_runtime.active && graph->transition_runtime.from_scene_id) {
        const sbs_scene_state_t *prev = sbs_scene_graph_get_scene(graph, graph->transition_runtime.from_scene_id);
        if (prev) {
            for (i = 0; i < prev->items->len && state->previous_item_count < SBS_COMP_SCENE_MAX_ITEMS; i++) {
                sbs_scene_item_state_t *item = g_ptr_array_index(prev->items, i);
                fill_comp_item(&graph->canvas, item, &state->previous_items[state->previous_item_count++],
                               graph->sources, prev->filters);
            }
        }
    }

    return SBS_OK;
}

sbs_scene_graph_t *sbs_scene_graph_new_default(void)
{
    sbs_scene_graph_t *graph = g_new0(sbs_scene_graph_t, 1);
    sbs_transition_create_params_t cut = { "trans-cut", SBS_TRANSITION_KIND_CUT, 0 };
    sbs_transition_create_params_t fade = { "trans-fade", SBS_TRANSITION_KIND_FADE, 2000 };

    graph->canvas.width = 1920;
    graph->canvas.height = 1080;
    graph->canvas.fps_num = 60;
    graph->canvas.fps_den = 1;
    graph->canvas.pixel_format = SBS_PIXEL_FORMAT_NV21;
    graph->canvas.colorimetry = SBS_COLORIMETRY_SDR;
    graph->canvas.color_mode = SBS_SCENE_COLOR_MODE_SDR;
    g_strlcpy(graph->canvas.background_color, "#000000", sizeof(graph->canvas.background_color));

    graph->sources = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, source_state_free);
    graph->scenes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, scene_state_free);
    graph->outputs = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, output_state_free);
    graph->transitions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, transition_state_free);

    sbs_scene_graph_create_transition(graph, &cut, NULL);
    sbs_scene_graph_create_transition(graph, &fade, NULL);
    graph->default_transition_id = g_strdup("trans-fade");
    graph->version = 1;

    return graph;
}

void sbs_scene_graph_free(sbs_scene_graph_t *graph)
{
    if (!graph) {
        return;
    }
    g_hash_table_destroy(graph->sources);
    g_hash_table_destroy(graph->scenes);
    g_hash_table_destroy(graph->outputs);
    g_hash_table_destroy(graph->transitions);
    g_free(graph->active_scene_id);
    g_free(graph->preview_scene_id);
    g_free(graph->default_transition_id);
    g_free(graph->transition_runtime.from_scene_id);
    g_free(graph->transition_runtime.to_scene_id);
    g_free(graph->transition_runtime.transition_id);
    g_free(graph);
}

sbs_source_state_t *sbs_scene_graph_get_source(const sbs_scene_graph_t *graph, const char *id)
{
    return graph && id ? g_hash_table_lookup(graph->sources, id) : NULL;
}

sbs_scene_state_t *sbs_scene_graph_get_scene(const sbs_scene_graph_t *graph, const char *id)
{
    return graph && id ? g_hash_table_lookup(graph->scenes, id) : NULL;
}

sbs_scene_item_state_t *sbs_scene_graph_get_item(const sbs_scene_graph_t *graph,
                                                 const char *scene_id,
                                                 const char *item_id)
{
    sbs_scene_state_t *scene;
    if (!graph || !scene_id || !item_id) {
        return NULL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    return scene_find_item(scene, item_id, NULL);
}

sbs_output_state_t *sbs_scene_graph_get_output(const sbs_scene_graph_t *graph, const char *id)
{
    return graph && id ? g_hash_table_lookup(graph->outputs, id) : NULL;
}

sbs_transition_state_t *sbs_scene_graph_get_transition(const sbs_scene_graph_t *graph, const char *id)
{
    return graph && id ? g_hash_table_lookup(graph->transitions, id) : NULL;
}

int sbs_scene_graph_create_source(sbs_scene_graph_t *graph,
                                  const sbs_source_create_params_t *params,
                                  sbs_source_state_t **out_source)
{
    sbs_source_state_t *source;

    if (!graph || !params || !valid_graph_id(params->id)) {
        return SBS_ERR_INVAL;
    }
    if (g_hash_table_contains(graph->sources, params->id)) {
        return SBS_ERR_INVAL;
    }

    source = g_new0(sbs_source_state_t, 1);
    source->id = g_strdup(params->id);
    source->name = g_strdup(params->name ? params->name : params->id);
    source->kind = params->kind;
    source->enabled = params->enabled;
    source->keep_alive = params->keep_alive;
    if (params->config) {
        source->config = str_map_copy(params->config);
    } else {
        source->config = str_map_new();
    }
    source->filters = g_ptr_array_new_with_free_func(filter_state_free);
    source->audio.volume = 1.0;
    source->audio.left_gain = 1.0;
    source->audio.right_gain = 1.0;
    source->runtime_state = g_strdup("created");
    sbs_frame_slot_init(&source->frame_slot);
    sbs_frame_slot_set_release_func(&source->frame_slot, sbs_frame_fds_release, NULL);
    source->slot_initialized = true;

    g_hash_table_insert(graph->sources, source->id, source);
    graph_bump_version(graph);
    if (out_source) {
        *out_source = source;
    }
    return SBS_OK;
}

int sbs_scene_graph_update_source(sbs_scene_graph_t *graph,
                                  const char *id,
                                  const sbs_source_update_params_t *params,
                                  sbs_source_state_t **out_source)
{
    sbs_source_state_t *source;

    if (!graph || !id || !params) {
        return SBS_ERR_INVAL;
    }
    source = g_hash_table_lookup(graph->sources, id);
    if (!source) {
        return SBS_ERR_NOT_FOUND;
    }

    if (params->set_name && params->name) {
        g_free(source->name);
        source->name = g_strdup(params->name);
    }
    if (params->set_enabled) {
        source->enabled = params->enabled;
    }
    if (params->set_keep_alive) {
        source->keep_alive = params->keep_alive;
    }
    if (params->set_config && params->config) {
        g_hash_table_destroy(source->config);
        source->config = str_map_copy(params->config);
    }

    graph_bump_version(graph);
    if (out_source) {
        *out_source = source;
    }
    return SBS_OK;
}

int sbs_scene_graph_remove_source(sbs_scene_graph_t *graph, const char *id)
{
    GHashTableIter iter;
    gpointer key, value;

    if (!graph || !id) {
        return SBS_ERR_INVAL;
    }
    if (!g_hash_table_contains(graph->sources, id)) {
        return SBS_ERR_NOT_FOUND;
    }

    g_hash_table_iter_init(&iter, graph->scenes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_scene_state_t *scene = value;
        guint i = 0;
        while (i < scene->items->len) {
            sbs_scene_item_state_t *item = g_ptr_array_index(scene->items, i);
            if (g_strcmp0(item->source_id, id) == 0) {
                g_ptr_array_remove_index(scene->items, i);
                continue;
            }
            i++;
        }
    }

    g_hash_table_steal(graph->sources, id);
    graph_bump_version(graph);
    return SBS_OK;
}

sbs_source_state_t *sbs_scene_graph_steal_source(sbs_scene_graph_t *graph,
                                                  const char *id)
{
    sbs_source_state_t *source;

    if (!graph || !id) {
        return NULL;
    }

    source = g_hash_table_lookup(graph->sources, id);
    if (!source) {
        return NULL;
    }

    g_hash_table_steal(graph->sources, id);
    return source;
}

int sbs_scene_graph_create_scene(sbs_scene_graph_t *graph,
                                 const sbs_scene_create_params_t *params,
                                 sbs_scene_state_t **out_scene)
{
    sbs_scene_state_t *scene;

    if (!graph || !params || !valid_graph_id(params->id)) {
        return SBS_ERR_INVAL;
    }
    if (g_hash_table_contains(graph->scenes, params->id)) {
        return SBS_ERR_INVAL;
    }

    scene = g_new0(sbs_scene_state_t, 1);
    scene->id = g_strdup(params->id);
    scene->name = g_strdup(params->name ? params->name : params->id);
    scene->items = g_ptr_array_new_with_free_func(scene_item_state_free);
    scene->filters = g_ptr_array_new_with_free_func(filter_state_free);

    g_hash_table_insert(graph->scenes, scene->id, scene);
    if (!graph->active_scene_id) {
        graph->active_scene_id = g_strdup(scene->id);
    }
    graph_bump_version(graph);
    if (out_scene) {
        *out_scene = scene;
    }
    return SBS_OK;
}

int sbs_scene_graph_update_scene_name(sbs_scene_graph_t *graph,
                                      const char *id,
                                      const char *name)
{
    sbs_scene_state_t *scene = sbs_scene_graph_get_scene(graph, id);
    if (!scene || !name) {
        return scene ? SBS_ERR_INVAL : SBS_ERR_NOT_FOUND;
    }
    g_free(scene->name);
    scene->name = g_strdup(name);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_remove_scene(sbs_scene_graph_t *graph, const char *id)
{
    if (!graph || !id) {
        return SBS_ERR_INVAL;
    }
    if (!g_hash_table_contains(graph->scenes, id)) {
        return SBS_ERR_NOT_FOUND;
    }
    if (g_strcmp0(graph->active_scene_id, id) == 0) {
        return SBS_ERR_INVAL;
    }
    if (g_strcmp0(graph->preview_scene_id, id) == 0) {
        g_clear_pointer(&graph->preview_scene_id, g_free);
    }
    g_hash_table_remove(graph->scenes, id);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_add_item(sbs_scene_graph_t *graph,
                             const char *scene_id,
                             const sbs_scene_item_create_params_t *params,
                             sbs_scene_item_state_t **out_item)
{
    sbs_scene_state_t *scene;
    sbs_scene_item_state_t *item;

    if (!graph || !scene_id || !params || !params->source_id) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) {
        return SBS_ERR_NOT_FOUND;
    }
    if (!sbs_scene_graph_get_source(graph, params->source_id)) {
        return SBS_ERR_NOT_FOUND;
    }
    if (params->id && scene_find_item(scene, params->id, NULL)) {
        return SBS_ERR_INVAL;
    }

    item = g_new0(sbs_scene_item_state_t, 1);
    item->id = g_strdup(params->id ? params->id : "item-auto");
    item->source_id = g_strdup(params->source_id);
    item->visible = params->visible;
    item->locked = params->locked;
    item->z_order = params->z_order;
    item->filters = g_ptr_array_new_with_free_func(filter_state_free);
    scene_item_transform_copy(&item->transform, &params->transform);
    if (params->audio) {
        audio_binding_copy(&item->audio, params->audio);
    } else {
        sbs_source_state_t *source = sbs_scene_graph_get_source(graph, params->source_id);
        audio_binding_copy(&item->audio, source ? &source->audio : NULL);
    }

    g_ptr_array_add(scene->items, item);
    scene_sort_items(scene);
    graph_bump_version(graph);
    if (out_item) {
        *out_item = item;
    }
    return SBS_OK;
}

int sbs_scene_graph_update_item(sbs_scene_graph_t *graph,
                                const char *scene_id,
                                const char *item_id,
                                const sbs_scene_item_update_params_t *params,
                                sbs_scene_item_state_t **out_item)
{
    sbs_scene_state_t *scene;
    sbs_scene_item_state_t *item;

    if (!graph || !scene_id || !item_id || !params) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) {
        return SBS_ERR_NOT_FOUND;
    }
    item = scene_find_item(scene, item_id, NULL);
    if (!item) {
        return SBS_ERR_NOT_FOUND;
    }

    if (params->set_visible) {
        item->visible = params->visible;
    }
    if (params->set_locked) {
        item->locked = params->locked;
    }
    if (params->set_z_order) {
        item->z_order = params->z_order;
    }
    if (params->set_transform) {
        scene_item_transform_clear(&item->transform);
        scene_item_transform_copy(&item->transform, &params->transform);
    }

    scene_sort_items(scene);
    graph_bump_version(graph);
    if (out_item) {
        *out_item = item;
    }
    return SBS_OK;
}

int sbs_scene_graph_update_item_audio(sbs_scene_graph_t *graph,
                                      const char *scene_id,
                                      const char *item_id,
                                      const sbs_audio_binding_t *audio,
                                      sbs_scene_item_state_t **out_item)
{
    sbs_scene_item_state_t *item;

    if (!graph || !scene_id || !item_id || !audio) {
        return SBS_ERR_INVAL;
    }
    item = sbs_scene_graph_get_item(graph, scene_id, item_id);
    if (!item) {
        return SBS_ERR_NOT_FOUND;
    }

    audio_binding_copy(&item->audio, audio);
    graph_bump_version(graph);
    if (out_item) {
        *out_item = item;
    }
    return SBS_OK;
}

int sbs_scene_graph_remove_item(sbs_scene_graph_t *graph,
                                const char *scene_id,
                                const char *item_id)
{
    sbs_scene_state_t *scene;
    guint index;

    if (!graph || !scene_id || !item_id) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) {
        return SBS_ERR_NOT_FOUND;
    }
    if (!scene_find_item(scene, item_id, &index)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_ptr_array_remove_index(scene->items, index);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_reorder_items(sbs_scene_graph_t *graph,
                                  const char *scene_id,
                                  const char * const *item_ids,
                                  size_t item_count)
{
    sbs_scene_state_t *scene;
    size_t i;

    if (!graph || !scene_id || !item_ids) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) {
        return SBS_ERR_NOT_FOUND;
    }
    if (item_count != scene->items->len) {
        return SBS_ERR_INVAL;
    }

    for (i = 0; i < item_count; i++) {
        sbs_scene_item_state_t *item = scene_find_item(scene, item_ids[i], NULL);
        if (!item) {
            return SBS_ERR_NOT_FOUND;
        }
        item->z_order = (int)(i * 10);
    }

    scene_sort_items(scene);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_add_filter(sbs_scene_graph_t *graph,
                               const char *source_id,
                               const sbs_filter_create_params_t *params,
                               sbs_filter_state_t **out_filter)
{
    sbs_source_state_t *source;
    sbs_filter_state_t *filter;
    if (!graph || !source_id || !params || !params->type) {
        return SBS_ERR_INVAL;
    }
    source = sbs_scene_graph_get_source(graph, source_id);
    if (!source) return SBS_ERR_NOT_FOUND;
    if (params->id && source_find_filter(source, params->id, NULL)) {
        return SBS_ERR_INVAL;
    }
    filter = g_new0(sbs_filter_state_t, 1);
    filter->id = g_strdup(params->id ? params->id : "filter-auto");
    filter->type = g_strdup(params->type);
    filter->enabled = params->enabled;
    filter->params = filter_params_copy(params->type, params->params);
    g_ptr_array_add(source->filters, filter);
    graph_bump_version(graph);
    if (out_filter) *out_filter = filter;
    return SBS_OK;
}

int sbs_scene_graph_add_scene_filter(sbs_scene_graph_t *graph,
                                     const char *scene_id,
                                     const sbs_filter_create_params_t *params,
                                     sbs_filter_state_t **out_filter)
{
    sbs_scene_state_t *scene;
    sbs_filter_state_t *filter;
    if (!graph || !scene_id || !params || !params->type) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) return SBS_ERR_NOT_FOUND;
    if (params->id && scene_find_filter(scene, params->id, NULL)) {
        return SBS_ERR_INVAL;
    }
    filter = g_new0(sbs_filter_state_t, 1);
    filter->id = g_strdup(params->id ? params->id : "filter-auto");
    filter->type = g_strdup(params->type);
    filter->enabled = params->enabled;
    filter->params = filter_params_copy(params->type, params->params);
    g_ptr_array_add(scene->filters, filter);
    graph_bump_version(graph);
    if (out_filter) *out_filter = filter;
    return SBS_OK;
}

int sbs_scene_graph_update_filter(sbs_scene_graph_t *graph,
                                  const char *source_id,
                                  const char *filter_id,
                                  const sbs_filter_update_params_t *params,
                                  sbs_filter_state_t **out_filter)
{
    sbs_source_state_t *source;
    sbs_filter_state_t *filter;
    if (!graph || !source_id || !filter_id || !params) {
        return SBS_ERR_INVAL;
    }
    source = sbs_scene_graph_get_source(graph, source_id);
    if (!source) return SBS_ERR_NOT_FOUND;
    filter = source_find_filter(source, filter_id, NULL);
    if (!filter) return SBS_ERR_NOT_FOUND;
    if (params->set_enabled) {
        filter->enabled = params->enabled;
    }
    if (params->params) {
        if (filter->params) {
            g_hash_table_destroy(filter->params);
        }
        filter->params = filter_params_copy(filter->type, params->params);
    }
    graph_bump_version(graph);
    if (out_filter) *out_filter = filter;
    return SBS_OK;
}

int sbs_scene_graph_update_scene_filter(sbs_scene_graph_t *graph,
                                        const char *scene_id,
                                        const char *filter_id,
                                        const sbs_filter_update_params_t *params,
                                        sbs_filter_state_t **out_filter)
{
    sbs_scene_state_t *scene;
    sbs_filter_state_t *filter;
    if (!graph || !scene_id || !filter_id || !params) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) return SBS_ERR_NOT_FOUND;
    filter = scene_find_filter(scene, filter_id, NULL);
    if (!filter) return SBS_ERR_NOT_FOUND;
    if (params->set_enabled) {
        filter->enabled = params->enabled;
    }
    if (params->params) {
        if (filter->params) {
            g_hash_table_destroy(filter->params);
        }
        filter->params = filter_params_copy(filter->type, params->params);
    }
    graph_bump_version(graph);
    if (out_filter) *out_filter = filter;
    return SBS_OK;
}

int sbs_scene_graph_remove_filter(sbs_scene_graph_t *graph,
                                  const char *source_id,
                                  const char *filter_id)
{
    sbs_source_state_t *source;
    guint index;
    if (!graph || !source_id || !filter_id) {
        return SBS_ERR_INVAL;
    }
    source = sbs_scene_graph_get_source(graph, source_id);
    if (!source) return SBS_ERR_NOT_FOUND;
    if (!source_find_filter(source, filter_id, &index)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_ptr_array_remove_index(source->filters, index);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_remove_scene_filter(sbs_scene_graph_t *graph,
                                        const char *scene_id,
                                        const char *filter_id)
{
    sbs_scene_state_t *scene;
    guint index;
    if (!graph || !scene_id || !filter_id) {
        return SBS_ERR_INVAL;
    }
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) return SBS_ERR_NOT_FOUND;
    if (!scene_find_filter(scene, filter_id, &index)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_ptr_array_remove_index(scene->filters, index);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_create_output(sbs_scene_graph_t *graph,
                                  const sbs_output_create_params_t *params,
                                  sbs_output_state_t **out_output)
{
    sbs_output_state_t *output;
    if (!graph || !params || !valid_graph_id(params->id)) {
        return SBS_ERR_INVAL;
    }
    if (g_hash_table_contains(graph->outputs, params->id)) {
        return SBS_ERR_INVAL;
    }
    output = g_new0(sbs_output_state_t, 1);
    output->id = g_strdup(params->id);
    output->name = g_strdup(params->name ? params->name : params->id);
    output->enabled = params->enabled;
    output->autostart = params->autostart;
    output->encoder = str_map_new();
    output->sinks = g_ptr_array_new_with_free_func(sink_state_free);
    output->runtime_state = g_strdup("created");
    g_hash_table_insert(graph->outputs, output->id, output);
    graph_bump_version(graph);
    if (out_output) {
        *out_output = output;
    }
    return SBS_OK;
}

int sbs_scene_graph_remove_output(sbs_scene_graph_t *graph, const char *id)
{
    if (!graph || !id) {
        return SBS_ERR_INVAL;
    }
    if (!g_hash_table_contains(graph->outputs, id)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_hash_table_remove(graph->outputs, id);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_create_transition(sbs_scene_graph_t *graph,
                                      const sbs_transition_create_params_t *params,
                                      sbs_transition_state_t **out_transition)
{
    sbs_transition_state_t *transition;
    if (!graph || !params || !valid_graph_id(params->id)) {
        return SBS_ERR_INVAL;
    }
    if (g_hash_table_contains(graph->transitions, params->id)) {
        return SBS_ERR_INVAL;
    }
    transition = g_new0(sbs_transition_state_t, 1);
    transition->id = g_strdup(params->id);
    transition->kind = params->kind;
    transition->duration_ms = params->duration_ms;
    transition->params = str_map_new();
    g_hash_table_insert(graph->transitions, transition->id, transition);
    graph_bump_version(graph);
    if (out_transition) {
        *out_transition = transition;
    }
    return SBS_OK;
}

int sbs_scene_graph_update_transition(sbs_scene_graph_t *graph,
                                      const char *transition_id,
                                      uint32_t duration_ms,
                                      sbs_transition_state_t **out_transition)
{
    sbs_transition_state_t *transition;

    if (!graph || !transition_id) {
        return SBS_ERR_INVAL;
    }
    transition = sbs_scene_graph_get_transition(graph, transition_id);
    if (!transition) {
        return SBS_ERR_NOT_FOUND;
    }
    transition->duration_ms = duration_ms;
    if (out_transition) {
        *out_transition = transition;
    }
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_set_default_transition(sbs_scene_graph_t *graph,
                                           const char *transition_id)
{
    if (!graph || !transition_id) {
        return SBS_ERR_INVAL;
    }
    if (!sbs_scene_graph_get_transition(graph, transition_id)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_free(graph->default_transition_id);
    graph->default_transition_id = g_strdup(transition_id);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_set_active_scene(sbs_scene_graph_t *graph,
                                     const char *scene_id,
                                     const char *transition_id)
{
    sbs_transition_state_t *transition;
    const char *resolved_transition;

    if (!graph || !scene_id) {
        return SBS_ERR_INVAL;
    }
    if (!sbs_scene_graph_get_scene(graph, scene_id)) {
        return SBS_ERR_NOT_FOUND;
    }

    resolved_transition = transition_id ? transition_id : graph->default_transition_id;
    transition = resolved_transition ? sbs_scene_graph_get_transition(graph, resolved_transition) : NULL;
    if (resolved_transition && !transition) {
        return SBS_ERR_NOT_FOUND;
    }

    g_free(graph->transition_runtime.from_scene_id);
    g_free(graph->transition_runtime.to_scene_id);
    g_free(graph->transition_runtime.transition_id);
    graph->transition_runtime.from_scene_id = dup_or_null(graph->active_scene_id);
    graph->transition_runtime.to_scene_id = g_strdup(scene_id);
    graph->transition_runtime.transition_id = dup_or_null(resolved_transition);
    graph->transition_runtime.kind = transition ? transition->kind : SBS_TRANSITION_KIND_CUT;
    graph->transition_runtime.duration_ms = transition ? transition->duration_ms : 0;
    graph->transition_runtime.progress = transition && transition->duration_ms > 0 ? 0.0 : 1.0;
    graph->transition_runtime.active = transition && transition->kind != SBS_TRANSITION_KIND_CUT && transition->duration_ms > 0;
    graph->transition_runtime.start_time_us = g_get_monotonic_time();

    g_free(graph->active_scene_id);
    graph->active_scene_id = g_strdup(scene_id);
    if (!graph->transition_runtime.active) {
        graph->transition_runtime.progress = 1.0;
    }
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_set_preview_scene(sbs_scene_graph_t *graph, const char *scene_id)
{
    if (!graph) {
        return SBS_ERR_INVAL;
    }
    if (scene_id && !sbs_scene_graph_get_scene(graph, scene_id)) {
        return SBS_ERR_NOT_FOUND;
    }
    g_free(graph->preview_scene_id);
    graph->preview_scene_id = dup_or_null(scene_id);
    graph_bump_version(graph);
    return SBS_OK;
}

int sbs_scene_graph_transition_to_preview(sbs_scene_graph_t *graph,
                                          const char *transition_id)
{
    if (!graph || !graph->preview_scene_id) {
        return SBS_ERR_INVAL;
    }
    return sbs_scene_graph_set_active_scene(graph, graph->preview_scene_id, transition_id);
}

void sbs_scene_graph_update_transition_runtime(sbs_scene_graph_t *graph, int64_t now_us)
{
    double elapsed_ms;

    if (!graph || !graph->transition_runtime.active) {
        return;
    }
    if (graph->transition_runtime.duration_ms == 0) {
        graph->transition_runtime.progress = 1.0;
        graph->transition_runtime.active = false;
        return;
    }

    elapsed_ms = (double)(now_us - graph->transition_runtime.start_time_us) / 1000.0;
    graph->transition_runtime.progress = elapsed_ms / (double)graph->transition_runtime.duration_ms;
    if (graph->transition_runtime.progress >= 1.0) {
        graph->transition_runtime.progress = 1.0;
        graph->transition_runtime.active = false;
    }
}

int sbs_scene_graph_count_source_refs(const sbs_scene_graph_t *graph, const char *source_id)
{
    GHashTableIter iter;
    gpointer key, value;
    int count = 0;

    if (!graph || !source_id) return 0;

    g_hash_table_iter_init(&iter, graph->scenes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const sbs_scene_state_t *scene = (const sbs_scene_state_t *)value;
        if (!scene->items) continue;
        for (guint i = 0; i < scene->items->len; i++) {
            sbs_scene_item_state_t *item = (sbs_scene_item_state_t *)g_ptr_array_index(scene->items, i);
            if (item->source_id && strcmp(item->source_id, source_id) == 0) {
                count++;
            }
        }
    }
    return count;
}
