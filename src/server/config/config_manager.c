#define SBS_LOG_COMP "config"

#include "sbs/config_manager.h"
#include "sbs/api_server.h"
#include "sbs/audio_mixer.h"
#include "sbs/encoder_manager.h"
#include "sbs/log.h"
#include "sbs/source_start_config.h"

#include <cjson/cJSON.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

struct sbs_config_manager {
    char *config_dir;
    char *state_path;
    char *override_path;
    guint save_timeout_id;
    sbs_api_server_t *pending_server;
};

static const char *json_str(cJSON *obj, const char *key);
static bool json_bool(cJSON *obj, const char *key, bool fallback);
static double json_num(cJSON *obj, const char *key, double fallback);

static double snap_quarter_rotation(double degrees)
{
    int quarter = (int)lround(degrees / 90.0);
    quarter = ((quarter % 4) + 4) % 4;
    return (double)(quarter * 90);
}

static void load_audio_binding(cJSON *audio_obj, sbs_audio_binding_t *audio, bool default_enabled)
{
    cJSON *eq;

    if (!audio) return;
    audio->enabled = json_bool(audio_obj, "enabled", default_enabled);
    g_free(audio->device);
    audio->device = g_strdup(json_str(audio_obj, "device"));
    audio->volume = json_num(audio_obj, "volume", 1.0);
    audio->left_gain = json_num(audio_obj, "left_gain", 1.0);
    audio->right_gain = json_num(audio_obj, "right_gain", 1.0);
    audio->delay_ms = (int32_t)json_num(audio_obj, "delay_ms", 0.0);
    audio->mute = json_bool(audio_obj, "mute", false);
    audio->monitor = json_bool(audio_obj, "monitor", false);

    eq = cJSON_GetObjectItemCaseSensitive(audio_obj, "eq_bands");
    for (uint32_t i = 0; i < G_N_ELEMENTS(audio->eq_bands); i++) {
        cJSON *band = cJSON_IsArray(eq) ? cJSON_GetArrayItem(eq, (int)i) : NULL;
        audio->eq_bands[i] = cJSON_IsNumber(band) ? band->valuedouble : 0.0;
    }
}

static void seed_default_graph(sbs_scene_graph_t *graph)
{
    sbs_source_create_params_t src_params = {
        .id = "default-src",
        .name = "Default Test Source",
        .kind = SBS_SOURCE_KIND_VIDEOTESTSRC,
        .enabled = true,
        .keep_alive = false,
    };
    sbs_scene_create_params_t scene_params = {
        .id = "scene-main",
        .name = "Main",
    };
    sbs_scene_item_create_params_t item_params = {
        .id = "item-default",
        .source_id = "default-src",
        .visible = true,
        .locked = false,
        .z_order = 0,
        .transform = {
            .position_x = 0,
            .position_y = 0,
            .width = 3840,
            .height = 2160,
            .bounds_type = "stretch",
            .alignment = "center",
            .opacity = 1.0,
        },
    };

    if (!graph) return;
    sbs_scene_graph_create_source(graph, &src_params, NULL);
    sbs_source_state_t *default_source = sbs_scene_graph_get_source(graph, "default-src");
    if (default_source) {
        default_source->audio.enabled = true;
        default_source->audio.device = g_strdup("hw:0,2");
        default_source->audio.volume = 1.0;
        default_source->audio.left_gain = 1.0;
        default_source->audio.right_gain = 1.0;
        default_source->audio.monitor = false;
        default_source->audio.mute = false;
    }
    sbs_scene_graph_create_scene(graph, &scene_params, NULL);
    sbs_scene_graph_add_item(graph, "scene-main", &item_params, NULL);
}

static int validate_bundle(cJSON *bundle)
{
    cJSON *state;
    cJSON *scenes;
    cJSON *sources;
    cJSON *scene;
    if (!cJSON_IsObject(bundle)) return SBS_ERR_INVAL;
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(bundle, "kind"))) return SBS_ERR_INVAL;
    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(bundle, "schema_version"))) return SBS_ERR_INVAL;
    state = cJSON_GetObjectItemCaseSensitive(bundle, "state");
    if (!cJSON_IsObject(state)) return SBS_ERR_INVAL;
    scenes = cJSON_GetObjectItemCaseSensitive(state, "scenes");
    sources = cJSON_GetObjectItemCaseSensitive(state, "sources");
    if (!cJSON_IsObject(scenes) || !cJSON_IsObject(sources)) return SBS_ERR_INVAL;
    for (cJSON *source = sources->child; source; source = source->next) {
        sbs_source_kind_t kind;
        if (!sbs_scene_graph_parse_source_kind(json_str(source, "type"), &kind)) {
            LOG_W("persisted source '%s' has unsupported type '%s'",
                  source->string ? source->string : "(unknown)",
                  json_str(source, "type") ? json_str(source, "type") : "(null)");
            return SBS_ERR_INVAL;
        }
    }
    for (scene = scenes->child; scene; scene = scene->next) {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(scene, "items");
        int i;
        if (!cJSON_IsArray(items)) return SBS_ERR_INVAL;
        for (i = 0; i < cJSON_GetArraySize(items); i++) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            const char *source_id = json_str(item, "source_id");
            if (!source_id || !cJSON_GetObjectItemCaseSensitive(sources, source_id)) {
                return SBS_ERR_INVAL;
            }
        }
    }
    return SBS_OK;
}

static int migrate_bundle(cJSON *bundle)
{
    cJSON *version = cJSON_GetObjectItemCaseSensitive(bundle, "schema_version");
    if (!cJSON_IsNumber(version)) return SBS_ERR_INVAL;
    if (version->valueint == 1) return SBS_OK;
    if (version->valueint < 1) {
        version->valueint = 1;
        version->valuedouble = 1;
        return SBS_OK;
    }
    return SBS_ERR_INVAL;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

static bool json_bool(cJSON *obj, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;
    return fallback;
}

static double json_num(cJSON *obj, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static GHashTable *json_object_to_map(cJSON *obj)
{
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    cJSON *child;
    if (!cJSON_IsObject(obj)) return map;
    for (child = obj->child; child; child = child->next) {
        if (child->string) {
            if (cJSON_IsString(child)) {
                g_hash_table_insert(map, g_strdup(child->string), g_strdup(cJSON_GetStringValue(child)));
            } else if (cJSON_IsBool(child)) {
                g_hash_table_insert(map, g_strdup(child->string), g_strdup(cJSON_IsTrue(child) ? "true" : "false"));
            } else if (cJSON_IsNumber(child)) {
                char buf[64];
                g_snprintf(buf, sizeof(buf), "%.4f", child->valuedouble);
                g_hash_table_insert(map, g_strdup(child->string), g_strdup(buf));
            }
        }
    }
    return map;
}

static void load_filter_array_for_source(sbs_scene_graph_t *graph,
                                          const char *source_id,
                                          cJSON *filters,
                                          bool only_if_empty)
{
    sbs_source_state_t *source;

    if (!graph || !source_id || !cJSON_IsArray(filters)) return;
    source = sbs_scene_graph_get_source(graph, source_id);
    if (!source) return;
    if (only_if_empty && source->filters && source->filters->len > 0) return;

    for (int i = 0; i < cJSON_GetArraySize(filters); i++) {
        cJSON *filter = cJSON_GetArrayItem(filters, i);
        sbs_filter_create_params_t fp = {
            json_str(filter, "id"),
            json_str(filter, "type"),
            json_bool(filter, "enabled", true),
            json_object_to_map(cJSON_GetObjectItemCaseSensitive(filter, "params")),
        };
        sbs_scene_graph_add_filter(graph, source_id, &fp, NULL);
        g_hash_table_destroy(fp.params);
    }
}

static void load_filter_array_for_scene(sbs_scene_graph_t *graph,
                                        const char *scene_id,
                                        cJSON *filters)
{
    sbs_scene_state_t *scene;

    if (!graph || !scene_id || !cJSON_IsArray(filters)) return;
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene) return;

    for (int i = 0; i < cJSON_GetArraySize(filters); i++) {
        cJSON *filter = cJSON_GetArrayItem(filters, i);
        sbs_filter_create_params_t fp = {
            json_str(filter, "id"),
            json_str(filter, "type"),
            json_bool(filter, "enabled", true),
            json_object_to_map(cJSON_GetObjectItemCaseSensitive(filter, "params")),
        };
        sbs_scene_graph_add_scene_filter(graph, scene_id, &fp, NULL);
        g_hash_table_destroy(fp.params);
    }
}

static void stop_runtime(sbs_api_server_t *server)
{
    GHashTableIter iter;
    gpointer key, value;

    if (server && server->encoder_mgr && server->scene_graph) {
        g_hash_table_iter_init(&iter, server->scene_graph->outputs);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            sbs_output_state_t *output = value;
            if (output && output->id &&
                sbs_encoder_manager_has_sink(server->encoder_mgr, output->id)) {
                sbs_encoder_manager_remove_sink(server->encoder_mgr, output->id);
            }
        }
    }
    if (server->output_sup) sbs_output_supervisor_shutdown_all(server->output_sup);
    if (server->source_sup) sbs_source_supervisor_shutdown_all(server->source_sup);
    if (server->audio) {
        sbs_audio_mixer_free(server->audio);
        server->audio = NULL;
    }
}

static void clear_scene_graph(sbs_scene_graph_t *graph)
{
    if (!graph) return;
    g_hash_table_remove_all(graph->sources);
    g_hash_table_remove_all(graph->scenes);
    g_hash_table_remove_all(graph->outputs);
    g_hash_table_remove_all(graph->transitions);
    g_clear_pointer(&graph->active_scene_id, g_free);
    g_clear_pointer(&graph->preview_scene_id, g_free);
    g_clear_pointer(&graph->default_transition_id, g_free);
    g_clear_pointer(&graph->transition_runtime.from_scene_id, g_free);
    g_clear_pointer(&graph->transition_runtime.to_scene_id, g_free);
    g_clear_pointer(&graph->transition_runtime.transition_id, g_free);
}

static const char *resolve_shared_codec_for_sink(const char *sink_type,
                                                 const char *requested_codec)
{
    if (requested_codec && *requested_codec)
        return requested_codec;
    if (g_strcmp0(sink_type, "rtmp") == 0)
        return "h264";
    return NULL;
}

static int ensure_restore_encoder_codec(sbs_api_server_t *server,
                                        const char *output_id,
                                        const char *sink_type,
                                        const char *requested_codec)
{
    const char *codec = resolve_shared_codec_for_sink(sink_type, requested_codec);
    sbs_encoder_config_t cfg = {0};
    const char *current_codec;
    uint32_t active_branches;

    if (!server || !server->encoder_mgr || !codec)
        return SBS_OK;

    sbs_encoder_manager_get_config(server->encoder_mgr, &cfg);
    current_codec = cfg.codec ? cfg.codec : "h265";
    if (g_strcmp0(current_codec, codec) == 0)
        return SBS_OK;

    active_branches = sbs_encoder_manager_sink_count(server->encoder_mgr);
    if (active_branches > 0) {
        LOG_W("cannot restore output '%s' with codec=%s while shared encoder is codec=%s with %u active branches",
              output_id ? output_id : "<unknown>", codec, current_codec, active_branches);
        return SBS_ERR_INVAL;
    }

    LOG_I("reconfiguring shared encoder for restored output '%s': codec=%s -> %s",
          output_id ? output_id : "<unknown>", current_codec, codec);
    cfg.codec = codec;
    return sbs_encoder_manager_update_config(server->encoder_mgr, &cfg);
}

static void restart_runtime_from_graph(sbs_api_server_t *server)
{
    GHashTableIter iter;
    gpointer key, value;
    if (!server || !server->scene_graph) return;

    if (server->preview) {
        sbs_preview_engine_set_source_format(server->preview,
                                             server->scene_graph->canvas.width,
                                             server->scene_graph->canvas.height,
                                             server->scene_graph->canvas.fps_num,
                                             server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10
                                                 ? SBS_PREVIEW_COLOR_MODE_HDR10
                                                 : SBS_PREVIEW_COLOR_MODE_SDR);
    }

    server->audio = sbs_audio_mixer_new(server->scene_graph);
    if (server->audio && sbs_audio_mixer_start(server->audio) == SBS_OK) {
        sbs_audio_mixer_on_scene_change(server->audio, server->scene_graph->active_scene_id);
        if (server->output_router) {
            sbs_output_router_set_audio_mixer(server->output_router, server->audio);
            sbs_output_router_set_color_mode(server->output_router,
                server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10
                    ? SBS_EXPORT_COLOR_HDR10 : SBS_EXPORT_COLOR_SDR);
        }
    }

    g_hash_table_iter_init(&iter, server->scene_graph->sources);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_source_state_t *source = value;
        if (source->enabled && server->source_sup) {
            sbs_source_start_config_t cfg = {0};
            int rc;
            if (source->kind == SBS_SOURCE_KIND_ALSA_AUDIO) {
                source->running = true;
                g_free(source->runtime_state);
                source->runtime_state = g_strdup("running");
                continue;
            }
            sbs_source_start_config_fill(server->scene_graph, &server->scene_graph->canvas, source, &cfg);
            rc = sbs_source_supervisor_start_source(server->source_sup, &cfg, &source->frame_slot);
            if (rc != SBS_OK) {
                LOG_E("failed to restore source '%s': %d", source->id, rc);
                source->running = false;
                g_free(source->runtime_state);
                source->runtime_state = g_strdup("error");
                continue;
            }
            source->running = true;
            source->frame_width = cfg.width;
            source->frame_height = cfg.height;
            source->color_depth = source->kind == SBS_SOURCE_KIND_VFMCAP &&
                                  server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? 10 : 8;
            source->hdr = false;
            g_strlcpy(source->color_space, source->color_depth > 8 ? "BT.2020" : "BT.709",
                      sizeof(source->color_space));
            g_strlcpy(source->hdr_eotf, "SDR", sizeof(source->hdr_eotf));
            g_free(source->runtime_state);
            source->runtime_state = g_strdup("starting");
        }
    }

    g_hash_table_iter_init(&iter, server->scene_graph->outputs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_output_state_t *output = value;
        if (output->enabled && output->autostart) {
            GHashTable *enc = output->encoder;
            const char *sink_type = enc ? g_hash_table_lookup(enc, "sink_type") : NULL;
            const char *srt_uri   = enc ? g_hash_table_lookup(enc, "srt_uri")   : NULL;
            const char *lat_str   = enc ? g_hash_table_lookup(enc, "srt_latency_ms") : NULL;
            const char *rtmp_uri  = enc ? g_hash_table_lookup(enc, "rtmp_uri")  : NULL;
            const char *rtmp_passcode = enc ? g_hash_table_lookup(enc, "rtmp_passcode") : NULL;
            const char *file_path = enc ? g_hash_table_lookup(enc, "file_path") : NULL;
            const char *file_path_mode = enc ? g_hash_table_lookup(enc, "file_path_mode") : NULL;
            const char *file_prefix = enc ? g_hash_table_lookup(enc, "file_prefix") : NULL;
            const char *file_container = enc ? g_hash_table_lookup(enc, "file_container") : NULL;
            const char *codec_str = enc ? g_hash_table_lookup(enc, "codec") : NULL;
            uint32_t srt_latency  = lat_str ? (uint32_t)strtoul(lat_str, NULL, 10) : 600;
            if (!sink_type) sink_type = "srt";
            if (!srt_uri)   srt_uri   = "srt://:8888";
            if (!file_path_mode) file_path_mode = "file";
            if (!file_prefix) file_prefix = "stream";
            if (!file_container) file_container = "ts";

            if (server->encoder_mgr) {
                sbs_sink_branch_config_t sink_cfg = {0};
                int rc;
                sink_cfg.output_id      = output->id;
                sink_cfg.sink_type      = sink_type;
                sink_cfg.srt_uri        = srt_uri;
                sink_cfg.srt_latency_ms = srt_latency;
                sink_cfg.rtmp_uri       = rtmp_uri;
                sink_cfg.rtmp_passcode  = rtmp_passcode;
                sink_cfg.file_path      = file_path;
                sink_cfg.file_path_mode = file_path_mode;
                sink_cfg.file_prefix    = file_prefix;
                sink_cfg.file_container = file_container;
                rc = ensure_restore_encoder_codec(server, output->id, sink_type, codec_str);
                if (rc != SBS_OK) {
                    LOG_E("failed to prepare encoder for restored output '%s': %d", output->id, rc);
                    output->running = false;
                    g_free(output->runtime_state);
                    output->runtime_state = g_strdup("error");
                    continue;
                }
                rc = sbs_encoder_manager_add_sink(server->encoder_mgr, &sink_cfg);
                if (rc != SBS_OK) {
                    LOG_E("failed to restore output '%s': %d", output->id, rc);
                    output->running = false;
                    g_free(output->runtime_state);
                    output->runtime_state = g_strdup("error");
                    continue;
                }
                output->running = true;
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("running");
            } else if (server->output_sup) {
                const char *brate_str  = enc ? g_hash_table_lookup(enc, "bitrate_kbps") : NULL;
                const char *gop_str    = enc ? g_hash_table_lookup(enc, "gop_size")     : NULL;
                uint32_t fps_num = server->scene_graph->canvas.fps_num;
                sbs_output_start_config_t cfg = {0};
                cfg.output_id     = output->id;
                cfg.width         = server->scene_graph->canvas.width;
                cfg.height        = server->scene_graph->canvas.height;
                cfg.framerate_num = fps_num;
                cfg.framerate_den = server->scene_graph->canvas.fps_den;
                cfg.codec         = codec_str  ? codec_str  : "h265";
                cfg.bitrate_kbps  = brate_str  ? (uint32_t)strtoul(brate_str, NULL, 10) : 10000;
                cfg.sink_type     = sink_type;
                cfg.srt_uri       = srt_uri;
                cfg.srt_latency_ms = srt_latency;
                cfg.rtmp_uri      = rtmp_uri;
                cfg.rtmp_passcode = rtmp_passcode;
                cfg.file_path     = file_path;
                cfg.file_path_mode = file_path_mode;
                cfg.file_prefix   = file_prefix;
                cfg.file_container = file_container;
                cfg.gop_size      = gop_str ? (uint32_t)strtoul(gop_str, NULL, 10) : fps_num;
                int rc = sbs_output_supervisor_start_output(server->output_sup, &cfg);
                if (rc != SBS_OK) {
                    LOG_E("failed to restore output '%s': %d", output->id, rc);
                    output->running = false;
                    g_free(output->runtime_state);
                    output->runtime_state = g_strdup("error");
                    continue;
                }
                output->running = true;
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("starting");
            }
        }
    }

    sbs_api_server_refresh_scene(server);
}

static gboolean restart_instance_cb(gpointer user_data)
{
    const char *reason = user_data ? user_data : "canvas config changed";
    LOG_I("exiting instance for supervised restart: %s", reason);
    exit(EXIT_SUCCESS);
    return G_SOURCE_REMOVE;
}

static void schedule_instance_restart(const char *reason)
{
    g_timeout_add(250, restart_instance_cb, (gpointer)reason);
}

static int reconfigure_canvas_runtime_from_graph(sbs_api_server_t *server)
{
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    sbs_export_color_mode_t color_mode;
    bool hdr10;
    bool comp_needs_reconfigure = false;

    if (!server || !server->scene_graph)
        return SBS_ERR_INVAL;

    width = server->scene_graph->canvas.width;
    height = server->scene_graph->canvas.height;
    fps_num = server->scene_graph->canvas.fps_num;
    fps_den = server->scene_graph->canvas.fps_den ? server->scene_graph->canvas.fps_den : 1;
    hdr10 = server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10;
    color_mode = hdr10 ? SBS_EXPORT_COLOR_HDR10 : SBS_EXPORT_COLOR_SDR;

    if (server->comp_thread) {
        uint32_t cur_w = 0;
        uint32_t cur_h = 0;
        uint32_t cur_fps = 0;
        sbs_export_color_mode_t cur_color = SBS_EXPORT_COLOR_SDR;
        sbs_compositor_thread_get_canvas_config(server->comp_thread,
                                                &cur_w, &cur_h, &cur_fps,
                                                &cur_color);
        comp_needs_reconfigure = cur_w != width || cur_h != height ||
                                 cur_fps != fps_num || cur_color != color_mode;
    }

    if (comp_needs_reconfigure) {
        LOG_I("canvas change requires supervised instance restart: %ux%u@%u/%u color=%s",
              width, height, fps_num, fps_den, hdr10 ? "hdr10" : "sdr");
        return SBS_ERR_WOULD_BLOCK;
    }

    if (server->encoder_mgr) {
        int rc = sbs_encoder_manager_reconfigure_video(server->encoder_mgr,
                                                       width, height,
                                                       fps_num, fps_den,
                                                       hdr10);
        if (rc != SBS_OK)
            return rc;
    }

    if (server->output_router)
        sbs_output_router_set_color_mode(server->output_router, color_mode);

    return SBS_OK;
}

static cJSON *build_preview_bundle(sbs_api_server_t *server)
{
    cJSON *preview;
    cJSON *encoder;

    if (!server || !server->preview)
        return NULL;

    encoder = sbs_preview_engine_serialize_encoder_config(
        server->preview, "preview-h264-webrtc");
    if (!encoder)
        return NULL;

    preview = cJSON_CreateObject();
    cJSON_AddItemToObject(preview, "encoder", encoder);
    return preview;
}

static void apply_preview_bundle(sbs_api_server_t *server, cJSON *bundle)
{
    cJSON *preview;
    cJSON *encoder;
    int rc;

    if (!server || !server->preview || !cJSON_IsObject(bundle))
        return;

    preview = cJSON_GetObjectItemCaseSensitive(bundle, "preview");
    encoder = cJSON_GetObjectItemCaseSensitive(preview, "encoder");
    if (!cJSON_IsObject(encoder))
        return;

    rc = sbs_preview_engine_apply_encoder_config(
        server->preview, "preview-h264-webrtc", encoder);
    if (rc != SBS_OK) {
        LOG_W("ignored invalid persisted preview encoder config: %d", rc);
    }
}

void sbs_config_manager_start_runtime(sbs_api_server_t *server)
{
    restart_runtime_from_graph(server);
}

static int apply_scene_graph_bundle(sbs_api_server_t *server, cJSON *bundle)
{
    cJSON *canvas, *sources, *scenes, *outputs, *transitions, *state, *audio;
    cJSON *entry;
    sbs_scene_graph_t *graph = server->scene_graph;
    if (!server || !graph || !cJSON_IsObject(bundle)) return SBS_ERR_INVAL;

    canvas = cJSON_GetObjectItemCaseSensitive(bundle, "canvas");
    sources = cJSON_GetObjectItemCaseSensitive(bundle, "sources");
    scenes = cJSON_GetObjectItemCaseSensitive(bundle, "scenes");
    outputs = cJSON_GetObjectItemCaseSensitive(bundle, "output_groups");
    transitions = cJSON_GetObjectItemCaseSensitive(bundle, "transitions");
    state = cJSON_GetObjectItemCaseSensitive(bundle, "state");
    audio = cJSON_GetObjectItemCaseSensitive(bundle, "audio");

    clear_scene_graph(graph);

    graph->canvas.width = (uint32_t)json_num(canvas, "width", 3840);
    graph->canvas.height = (uint32_t)json_num(canvas, "height", 2160);
    graph->canvas.fps_num = (uint32_t)json_num(canvas, "fps_num", 60);
    graph->canvas.fps_den = (uint32_t)json_num(canvas, "fps_den", 1);
    g_strlcpy(graph->canvas.background_color, json_str(canvas, "background_color") ? json_str(canvas, "background_color") : "#000000", sizeof(graph->canvas.background_color));
    graph->canvas.color_mode = g_strcmp0(json_str(canvas, "color_mode"), "hdr10") == 0 ? SBS_SCENE_COLOR_MODE_HDR10 : SBS_SCENE_COLOR_MODE_SDR;

    for (entry = sources ? sources->child : NULL; entry; entry = entry->next) {
        sbs_source_kind_t kind;
        if (!sbs_scene_graph_parse_source_kind(json_str(entry, "type"), &kind)) return SBS_ERR_INVAL;
        sbs_source_create_params_t create = {
            .id = entry->string,
            .name = json_str(entry, "name"),
            .kind = kind,
            .enabled = json_bool(entry, "enabled", true),
            .keep_alive = json_bool(entry, "keep_alive", false),
        };
        sbs_source_state_t *source = NULL;
        sbs_scene_graph_create_source(graph, &create, &source);
        if (source && (create.kind == SBS_SOURCE_KIND_STREAMBOXSRC ||
                       create.kind == SBS_SOURCE_KIND_V4L2SRC ||
                       create.kind == SBS_SOURCE_KIND_VFMCAP)) {
            source->keep_alive = true;
        }
        if (source) {
            cJSON *cfg = cJSON_GetObjectItemCaseSensitive(entry, "config");
            cJSON *audio_obj = cJSON_GetObjectItemCaseSensitive(entry, "audio");
            cJSON *filters = cJSON_GetObjectItemCaseSensitive(entry, "filters");
            g_hash_table_destroy(source->config);
            source->config = json_object_to_map(cfg);
            load_filter_array_for_source(graph, source->id, filters, false);
            load_audio_binding(audio_obj, &source->audio, create.kind == SBS_SOURCE_KIND_ALSA_AUDIO);
            if (create.kind == SBS_SOURCE_KIND_ALSA_AUDIO && !source->audio.device) {
                const char *device = source->config ? g_hash_table_lookup(source->config, "device") : NULL;
                source->audio.device = g_strdup(device && device[0] ? device : "hw:0,2");
            }
        }
    }

    for (entry = scenes ? scenes->child : NULL; entry; entry = entry->next) {
        sbs_scene_create_params_t create = { entry->string, json_str(entry, "name") };
        sbs_scene_state_t *scene = NULL;
        cJSON *items = cJSON_GetObjectItemCaseSensitive(entry, "items");
        sbs_scene_graph_create_scene(graph, &create, &scene);
        if (scene) {
            load_filter_array_for_scene(graph, scene->id,
                                        cJSON_GetObjectItemCaseSensitive(entry, "filters"));
        }
        if (scene && cJSON_IsArray(items)) {
            int i;
            for (i = 0; i < cJSON_GetArraySize(items); i++) {
                cJSON *item = cJSON_GetArrayItem(items, i);
                cJSON *transform = cJSON_GetObjectItemCaseSensitive(item, "transform");
                cJSON *audio_obj = cJSON_GetObjectItemCaseSensitive(item, "audio");
                sbs_audio_binding_t item_audio = {0};
                sbs_scene_item_create_params_t ip = {0};
                ip.id = json_str(item, "id");
                ip.source_id = json_str(item, "source_id");
                ip.visible = json_bool(item, "visible", true);
                ip.locked = json_bool(item, "locked", false);
                ip.z_order = (int)json_num(item, "z_order", 0);
                ip.transform.position_x = (int)json_num(transform, "position_x", 0);
                ip.transform.position_y = (int)json_num(transform, "position_y", 0);
                ip.transform.width = (int)json_num(transform, "width", 640);
                ip.transform.height = (int)json_num(transform, "height", 360);
                ip.transform.crop_top = (int)json_num(transform, "crop_top", 0);
                ip.transform.crop_bottom = (int)json_num(transform, "crop_bottom", 0);
                ip.transform.crop_left = (int)json_num(transform, "crop_left", 0);
                ip.transform.crop_right = (int)json_num(transform, "crop_right", 0);
                ip.transform.rotation_deg = snap_quarter_rotation(json_num(transform, "rotation_deg", 0.0));
                ip.transform.flip_horizontal = json_bool(transform, "flip_horizontal", false);
                ip.transform.flip_vertical = json_bool(transform, "flip_vertical", false);
                ip.transform.bounds_type = (char *)(json_str(transform, "bounds_type") ? json_str(transform, "bounds_type") : "stretch");
                ip.transform.alignment = (char *)(json_str(transform, "alignment") ? json_str(transform, "alignment") : "center");
                ip.transform.opacity = json_num(transform, "opacity", 1.0);
                if (cJSON_IsObject(audio_obj)) {
                    load_audio_binding(audio_obj, &item_audio, false);
                    ip.audio = &item_audio;
                }
                sbs_scene_item_state_t *created_item = NULL;
                sbs_scene_graph_add_item(graph, create.id, &ip, &created_item);
                g_free(item_audio.device);
                if (created_item) {
                    cJSON *filters = cJSON_GetObjectItemCaseSensitive(item, "filters");
                    load_filter_array_for_source(graph, created_item->source_id, filters, true);
                }
            }
        }
    }

    for (entry = transitions ? transitions->child : NULL; entry; entry = entry->next) {
        uint32_t duration_ms = (uint32_t)json_num(entry, "duration_ms", 0);
        if (g_strcmp0(entry->string, "trans-fade") == 0 && duration_ms == 500)
            duration_ms = 2000;
        sbs_transition_create_params_t tp = { entry->string, g_strcmp0(json_str(entry, "type"), "fade") == 0 ? SBS_TRANSITION_KIND_FADE : (g_strcmp0(json_str(entry, "type"), "slide") == 0 ? SBS_TRANSITION_KIND_SLIDE : SBS_TRANSITION_KIND_CUT), duration_ms };
        sbs_scene_graph_create_transition(graph, &tp, NULL);
    }

    for (entry = outputs ? outputs->child : NULL; entry; entry = entry->next) {
        sbs_output_create_params_t op = { entry->string, json_str(entry, "name"), json_bool(entry, "enabled", true), json_bool(entry, "autostart", false) };
        sbs_output_state_t *output = NULL;
        sbs_scene_graph_create_output(graph, &op, &output);
        if (output) {
            cJSON *encoder = cJSON_GetObjectItemCaseSensitive(entry, "encoder");
            g_hash_table_destroy(output->encoder);
            output->encoder = json_object_to_map(encoder);
        }
    }

    if (state && cJSON_IsObject(state)) {
        if (json_str(state, "active_scene_id")) {
            g_free(graph->active_scene_id);
            graph->active_scene_id = g_strdup(json_str(state, "active_scene_id"));
        }
        if (json_str(state, "preview_scene_id")) {
            g_free(graph->preview_scene_id);
            graph->preview_scene_id = g_strdup(json_str(state, "preview_scene_id"));
        }
        if (json_str(state, "transition_id")) {
            g_free(graph->default_transition_id);
            graph->default_transition_id = g_strdup(json_str(state, "transition_id"));
        }
    }

    if (audio && server->audio) {
        double master_eq[10] = {0};
        cJSON *eq = cJSON_GetObjectItemCaseSensitive(audio, "master_eq_bands");
        if (cJSON_IsArray(eq)) {
            for (uint32_t i = 0; i < G_N_ELEMENTS(master_eq); i++) {
                cJSON *band = cJSON_GetArrayItem(eq, (int)i);
                if (cJSON_IsNumber(band)) {
                    master_eq[i] = band->valuedouble;
                }
            }
        }
        sbs_audio_mixer_set_master(server->audio,
                                   json_num(audio, "master_volume", 1.0),
                                   json_bool(audio, "master_mute", false),
                                   json_num(audio, "master_left_gain", 1.0),
                                   json_num(audio, "master_right_gain", 1.0),
                                   cJSON_IsArray(eq) ? master_eq : NULL,
                                   G_N_ELEMENTS(master_eq));
    }

    return SBS_OK;
}

static gboolean save_timeout_cb(gpointer user_data)
{
    sbs_config_manager_t *mgr = user_data;
    sbs_api_server_t *server = mgr ? mgr->pending_server : NULL;
    cJSON *bundle;
    if (!mgr || !server) return G_SOURCE_REMOVE;
    bundle = sbs_config_manager_build_bundle(server);
    if (bundle) {
        sbs_config_manager_save_bundle(mgr, bundle);
        cJSON_Delete(bundle);
    }
    mgr->save_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

sbs_config_manager_t *sbs_config_manager_new(const char *config_dir)
{
    sbs_config_manager_t *mgr = g_new0(sbs_config_manager_t, 1);
    mgr->config_dir = g_strdup(config_dir ? config_dir : "/var/lib/sbs");
    mgr->state_path = g_build_filename(mgr->config_dir, "state.json", NULL);
    mgr->override_path = g_strdup(g_getenv("SBS_SYSTEM_CONFIG") ? g_getenv("SBS_SYSTEM_CONFIG") : "/etc/sbs/sbs.conf");
    g_mkdir_with_parents(mgr->config_dir, 0755);
    return mgr;
}

void sbs_config_manager_free(sbs_config_manager_t *mgr)
{
    if (!mgr) return;
    if (mgr->save_timeout_id) g_source_remove(mgr->save_timeout_id);
    g_free(mgr->config_dir);
    g_free(mgr->state_path);
    g_free(mgr->override_path);
    g_free(mgr);
}

const char *sbs_config_manager_config_dir(const sbs_config_manager_t *mgr)
{
    return mgr ? mgr->config_dir : NULL;
}

cJSON *sbs_config_manager_build_bundle(sbs_api_server_t *server)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *preview;
    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddItemToObject(bundle, "state", sbs_scene_graph_serialize_full_state(server->scene_graph));
    preview = build_preview_bundle(server);
    if (preview) {
        cJSON_AddItemToObject(bundle, "preview", preview);
    }
    if (server->audio) {
        cJSON_AddItemToObject(bundle, "audio", sbs_audio_mixer_serialize_state(server->audio));
    }
    return bundle;
}

int sbs_config_manager_load_bundle(sbs_config_manager_t *mgr, cJSON **out_bundle)
{
    gchar *contents = NULL;
    gsize len = 0;
    cJSON *bundle;
    if (!mgr || !out_bundle) return SBS_ERR_INVAL;
    if (!g_file_get_contents(mgr->state_path, &contents, &len, NULL)) {
        return SBS_ERR_NOT_FOUND;
    }
    (void)len;
    bundle = cJSON_Parse(contents);
    g_free(contents);
    if (!bundle) return SBS_ERR_INVAL;
    if (migrate_bundle(bundle) != SBS_OK || validate_bundle(bundle) != SBS_OK) {
        LOG_W("persisted config bundle invalid: %s", mgr->state_path);
        cJSON_Delete(bundle);
        return SBS_ERR_INVAL;
    }
    LOG_I("loaded persisted config bundle: %s", mgr->state_path);
    *out_bundle = bundle;
    return SBS_OK;
}

int sbs_config_manager_save_bundle(sbs_config_manager_t *mgr, cJSON *bundle)
{
    char *json;
    char *tmp_path;
    int fd;
    if (!mgr || !bundle) return SBS_ERR_INVAL;
    json = cJSON_PrintUnformatted(bundle);
    if (!json) return SBS_ERR_IO;
    tmp_path = g_strdup_printf("%s.tmp", mgr->state_path);
    fd = g_open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        g_free(json); g_free(tmp_path); return SBS_ERR_IO;
    }
    if (write(fd, json, strlen(json)) < 0 || fsync(fd) < 0) {
        close(fd); unlink(tmp_path); g_free(json); g_free(tmp_path); return SBS_ERR_IO;
    }
    close(fd);
    if (rename(tmp_path, mgr->state_path) < 0) {
        unlink(tmp_path); g_free(json); g_free(tmp_path); return SBS_ERR_IO;
    }
    g_free(json);
    g_free(tmp_path);
    return SBS_OK;
}

void sbs_config_manager_mark_dirty(sbs_config_manager_t *mgr, sbs_api_server_t *server)
{
    if (!mgr || !server) return;
    mgr->pending_server = server;
    if (mgr->save_timeout_id) g_source_remove(mgr->save_timeout_id);
    mgr->save_timeout_id = g_timeout_add(500, save_timeout_cb, mgr);
}

int sbs_config_manager_apply_bundle(sbs_config_manager_t *mgr, sbs_api_server_t *server, cJSON *bundle)
{
    cJSON *state = cJSON_GetObjectItemCaseSensitive(bundle, "state");
    int rc;
    if (!mgr || !server || !cJSON_IsObject(state)) return SBS_ERR_INVAL;
    if (migrate_bundle(bundle) != SBS_OK || validate_bundle(bundle) != SBS_OK) return SBS_ERR_INVAL;
    LOG_I("applying persisted config bundle");
    stop_runtime(server);
    if (apply_scene_graph_bundle(server, state) != SBS_OK) {
        LOG_W("failed to apply scene graph state from persisted config");
        return SBS_ERR_INVAL;
    }
    rc = reconfigure_canvas_runtime_from_graph(server);
    if (rc == SBS_ERR_WOULD_BLOCK) {
        int save_rc = sbs_config_manager_save_bundle(mgr, bundle);
        if (save_rc == SBS_OK)
            schedule_instance_restart("canvas config changed");
        return save_rc;
    }
    if (rc != SBS_OK) {
        LOG_W("failed to reconfigure runtime canvas from persisted config");
        return SBS_ERR_INVAL;
    }
    restart_runtime_from_graph(server);
    apply_preview_bundle(server, bundle);
    return sbs_config_manager_save_bundle(mgr, bundle);
}

int sbs_config_manager_reset(sbs_config_manager_t *mgr, sbs_api_server_t *server)
{
    cJSON *bundle;
    int rc;
    if (!mgr || !server) return SBS_ERR_INVAL;
    stop_runtime(server);
    sbs_scene_graph_free(server->scene_graph);
    server->scene_graph = sbs_scene_graph_new_default();
    seed_default_graph(server->scene_graph);
    if (g_file_test(mgr->state_path, G_FILE_TEST_EXISTS)) unlink(mgr->state_path);
    bundle = sbs_config_manager_build_bundle(server);
    sbs_config_manager_save_bundle(mgr, bundle);
    cJSON_Delete(bundle);
    rc = reconfigure_canvas_runtime_from_graph(server);
    if (rc == SBS_ERR_WOULD_BLOCK) {
        schedule_instance_restart("config reset changed canvas");
        return SBS_OK;
    }
    if (rc != SBS_OK) {
        LOG_W("failed to reconfigure runtime canvas during reset");
        return SBS_ERR_INVAL;
    }
    restart_runtime_from_graph(server);
    return SBS_OK;
}

int sbs_config_manager_apply_system_overrides(sbs_config_manager_t *mgr,
                                              sbs_scene_graph_t *graph,
                                              uint16_t *api_port)
{
    GKeyFile *kf;
    GError *error = NULL;
    if (!mgr || !graph) return SBS_ERR_INVAL;
    if (!g_file_test(mgr->override_path, G_FILE_TEST_EXISTS)) return SBS_OK;
    kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, mgr->override_path, G_KEY_FILE_NONE, &error)) {
        if (error) g_error_free(error);
        g_key_file_unref(kf);
        return SBS_ERR_INVAL;
    }
    if (g_key_file_has_group(kf, "canvas")) {
        if (g_key_file_has_key(kf, "canvas", "width", NULL)) graph->canvas.width = g_key_file_get_integer(kf, "canvas", "width", NULL);
        if (g_key_file_has_key(kf, "canvas", "height", NULL)) graph->canvas.height = g_key_file_get_integer(kf, "canvas", "height", NULL);
        if (g_key_file_has_key(kf, "canvas", "fps_num", NULL)) graph->canvas.fps_num = g_key_file_get_integer(kf, "canvas", "fps_num", NULL);
        if (g_key_file_has_key(kf, "canvas", "fps_den", NULL)) graph->canvas.fps_den = g_key_file_get_integer(kf, "canvas", "fps_den", NULL);
        if (g_key_file_has_key(kf, "canvas", "background_color", NULL)) {
            char *bg = g_key_file_get_string(kf, "canvas", "background_color", NULL);
            g_strlcpy(graph->canvas.background_color, bg ? bg : "#000000", sizeof(graph->canvas.background_color));
            g_free(bg);
        }
    }
    if (g_key_file_has_group(kf, "server") && api_port && g_key_file_has_key(kf, "server", "api_port", NULL)) {
        *api_port = (uint16_t)g_key_file_get_integer(kf, "server", "api_port", NULL);
    }
    if (g_key_file_has_group(kf, "audio") && g_key_file_has_key(kf, "audio", "device", NULL)) {
        char *device = g_key_file_get_string(kf, "audio", "device", NULL);
        sbs_source_state_t *src = sbs_scene_graph_get_source(graph, "default-src");
        if (src) {
            g_free(src->audio.device);
            src->audio.device = device;
            device = NULL;
        }
        g_free(device);
    }
    g_key_file_unref(kf);
    return SBS_OK;
}

int sbs_config_manager_preload_canvas(sbs_config_manager_t *mgr,
                                       sbs_scene_graph_t *graph)
{
    if (!mgr || !graph) return -1;

    char *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(mgr->state_path, &contents, &len, NULL)) {
        LOG_I("no persisted state to preload canvas from");
        return 0;  /* not an error – just no saved state yet */
    }

    cJSON *root = cJSON_Parse(contents);
    g_free(contents);
    if (!root) {
        LOG_W("preload_canvas: failed to parse %s", mgr->state_path);
        return -1;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    cJSON *canvas = cJSON_GetObjectItemCaseSensitive(
        state ? state : root, "canvas");
    if (canvas && cJSON_IsObject(canvas)) {
        graph->canvas.width   = (uint32_t)json_num(canvas, "width",   graph->canvas.width);
        graph->canvas.height  = (uint32_t)json_num(canvas, "height",  graph->canvas.height);
        graph->canvas.fps_num = (uint32_t)json_num(canvas, "fps_num", graph->canvas.fps_num);
        graph->canvas.fps_den = (uint32_t)json_num(canvas, "fps_den", graph->canvas.fps_den);
        const char *cm = json_str(canvas, "color_mode");
        if (cm) {
            graph->canvas.color_mode = (strcmp(cm, "hdr10") == 0)
                ? SBS_SCENE_COLOR_MODE_HDR10 : SBS_SCENE_COLOR_MODE_SDR;
        }
        LOG_I("preloaded canvas: %ux%u@%u/%u color_mode=%s",
              graph->canvas.width, graph->canvas.height,
              graph->canvas.fps_num, graph->canvas.fps_den,
              graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? "hdr10" : "sdr");
    } else {
        LOG_I("preload_canvas: no canvas object in state.json");
    }

    cJSON_Delete(root);
    return 0;
}
