#define SBS_LOG_COMP "api-source"

#include "sbs/api_server.h"
#include "sbs/log.h"
#include "sbs/source_start_config.h"
#include "sbs/v4l2_discovery.h"

#include <glib/gstdio.h>
#include <unistd.h>

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

static cJSON *source_field_string(const char *key, const char *label, const char *def)
{
    cJSON *field = cJSON_CreateObject();
    cJSON_AddStringToObject(field, "key", key);
    cJSON_AddStringToObject(field, "label", label);
    cJSON_AddStringToObject(field, "type", "string");
    cJSON_AddStringToObject(field, "default", def ? def : "");
    return field;
}

static cJSON *source_field_asset(const char *key, const char *label, const char *def,
                                 const char *asset_kind)
{
    cJSON *field = source_field_string(key, label, def);
    cJSON_AddStringToObject(field, "asset_kind", asset_kind);
    return field;
}

static cJSON *source_field_bool(const char *key, const char *label, bool def)
{
    cJSON *field = cJSON_CreateObject();
    cJSON_AddStringToObject(field, "key", key);
    cJSON_AddStringToObject(field, "label", label);
    cJSON_AddStringToObject(field, "type", "boolean");
    cJSON_AddBoolToObject(field, "default", def);
    return field;
}

static cJSON *source_field_select(const char *key, const char *label, const char *def,
                                  const char * const *options)
{
    cJSON *field = cJSON_CreateObject();
    cJSON *opts = cJSON_CreateArray();
    cJSON_AddStringToObject(field, "key", key);
    cJSON_AddStringToObject(field, "label", label);
    cJSON_AddStringToObject(field, "type", "select");
    cJSON_AddStringToObject(field, "default", def ? def : "");
    if (options) {
        for (uint32_t i = 0; options[i]; i++) {
            cJSON_AddItemToArray(opts, cJSON_CreateString(options[i]));
        }
    }
    cJSON_AddItemToObject(field, "options", opts);
    return field;
}

static cJSON *create_source_kind_json(sbs_source_kind_t kind, bool include_fields)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *fields = cJSON_CreateArray();
    static const char * const test_patterns[] = {
        "smpte", "ball", "bars", "snow", "red", "green", "blue", "black", "white",
        "blink", "circular", "pinwheel", "zone-plate", "gamut", "chroma-zone-plate", NULL
    };
    static const char * const streambox_modes[] = { "hdmirx", "vdin0", "vdin1", "test", NULL };
    static const char * const streambox_formats[] = { "nv12", "p010", NULL };
    static const char * const vfmcap_formats[] = { "raw", "nv12", "p010", NULL };
    static const char * const text_fonts[] = {
        "Liberation Sans", "Liberation Serif", "Liberation Mono", "Cantarell",
        "DejaVu Sans", "DejaVu Serif", "Noto Sans", "Noto Serif", NULL
    };
    static const char * const text_align[] = { "left", "center", "right", NULL };

    switch (kind) {
    case SBS_SOURCE_KIND_VIDEOTESTSRC:
        cJSON_AddStringToObject(obj, "id", "videotestsrc");
        cJSON_AddStringToObject(obj, "name", "Test Pattern");
        cJSON_AddStringToObject(obj, "summary", "GStreamer video test source with configurable patterns");
        cJSON_AddBoolToObject(obj, "pausable", true);
        cJSON_AddItemToArray(fields, source_field_select("pattern", "Pattern", "smpte", test_patterns));
        cJSON_AddItemToArray(fields, source_field_string("width", "Width", "1280"));
        cJSON_AddItemToArray(fields, source_field_string("height", "Height", "720"));
        cJSON_AddItemToArray(fields, source_field_string("fps", "FPS", "30"));
        break;
    case SBS_SOURCE_KIND_STREAMBOXSRC:
        cJSON_AddStringToObject(obj, "id", "streamboxsrc");
        cJSON_AddStringToObject(obj, "name", "StreamBox Capture");
        cJSON_AddStringToObject(obj, "summary", "Hardware-backed StreamBox capture source");
        cJSON_AddBoolToObject(obj, "pausable", false);
        cJSON_AddItemToArray(fields, source_field_select("capture_mode", "Capture Mode", "hdmirx", streambox_modes));
        cJSON_AddItemToArray(fields, source_field_select("output_format", "Output Format", "nv12", streambox_formats));
        break;
    case SBS_SOURCE_KIND_V4L2SRC:
        cJSON_AddStringToObject(obj, "id", "v4l2src");
        cJSON_AddStringToObject(obj, "name", "V4L2 Device");
        cJSON_AddStringToObject(obj, "summary", "Linux V4L2 video capture device");
        cJSON_AddBoolToObject(obj, "pausable", false);
        cJSON_AddItemToArray(fields, source_field_string("device_path", "Device Path", "/dev/video0"));
        break;
    case SBS_SOURCE_KIND_URIDECODEBIN:
        cJSON_AddStringToObject(obj, "id", "uridecodebin");
        cJSON_AddStringToObject(obj, "name", "Media File / URI");
        cJSON_AddStringToObject(obj, "summary", "URI-based media decoder for files and network streams");
        cJSON_AddBoolToObject(obj, "pausable", true);
        cJSON_AddItemToArray(fields, source_field_asset("uri", "URI / Media File", "", "media"));
        cJSON_AddItemToArray(fields, source_field_bool("loop", "Loop", true));
        break;
    case SBS_SOURCE_KIND_IMAGE:
        cJSON_AddStringToObject(obj, "id", "image");
        cJSON_AddStringToObject(obj, "name", "Image");
        cJSON_AddStringToObject(obj, "summary", "Static image source from a local path or URI");
        cJSON_AddBoolToObject(obj, "pausable", true);
        cJSON_AddItemToArray(fields, source_field_asset("path", "Image Path", "", "image"));
        cJSON_AddItemToArray(fields, source_field_string("width", "Width", "1280"));
        cJSON_AddItemToArray(fields, source_field_string("height", "Height", "720"));
        cJSON_AddItemToArray(fields, source_field_string("fps", "FPS", "1"));
        cJSON_AddItemToArray(fields, source_field_bool("loop", "Loop", true));
        break;
    case SBS_SOURCE_KIND_TEXT:
        cJSON_AddStringToObject(obj, "id", "text");
        cJSON_AddStringToObject(obj, "name", "Text Overlay");
        cJSON_AddStringToObject(obj, "summary", "Transparent text overlay with built-in free fonts or uploaded font files");
        cJSON_AddBoolToObject(obj, "pausable", true);
        cJSON_AddItemToArray(fields, source_field_string("text", "Text", "StreamBox"));
        cJSON_AddItemToArray(fields, source_field_select("font_family", "Font", "Liberation Sans", text_fonts));
        cJSON_AddItemToArray(fields, source_field_asset("font_path", "Custom Font File", "", "font"));
        cJSON_AddItemToArray(fields, source_field_string("font_size", "Font Size", "72"));
        cJSON_AddItemToArray(fields, source_field_string("text_color", "Text Color", "#ffffffff"));
        cJSON_AddItemToArray(fields, source_field_select("text_align", "Align", "left", text_align));
        cJSON_AddItemToArray(fields, source_field_string("width", "Width", "1280"));
        cJSON_AddItemToArray(fields, source_field_string("height", "Height", "256"));
        break;
    case SBS_SOURCE_KIND_VFMCAP:
        cJSON_AddStringToObject(obj, "id", "vfmcap");
        cJSON_AddStringToObject(obj, "name", "VFM Capture");
        cJSON_AddStringToObject(obj, "summary", "Direct libvfmcap HDMI capture passthrough");
        cJSON_AddBoolToObject(obj, "pausable", false);
        cJSON_AddItemToArray(fields, source_field_string("device", "Device Path", "/dev/video_cap"));
        cJSON_AddItemToArray(fields, source_field_select("output_format", "Output Format", "raw", vfmcap_formats));
        break;
    case SBS_SOURCE_KIND_ALSA_AUDIO:
        cJSON_AddStringToObject(obj, "id", "alsa_audio");
        cJSON_AddStringToObject(obj, "name", "ALSA Audio Input");
        cJSON_AddStringToObject(obj, "summary", "Audio-only input from an ALSA PCM device");
        cJSON_AddBoolToObject(obj, "pausable", false);
        cJSON_AddItemToArray(fields, source_field_string("device", "ALSA Device", "hw:0,2"));
        break;
    default:
        cJSON_Delete(fields);
        cJSON_Delete(obj);
        return NULL;
    }

    if (include_fields) {
        cJSON_AddItemToObject(obj, "fields", fields);
    } else {
        cJSON_Delete(fields);
    }
    return obj;
}

static GHashTable *json_object_to_str_map(cJSON *obj)
{
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    cJSON *entry;
    if (!cJSON_IsObject(obj)) return map;
    for (entry = obj->child; entry; entry = entry->next) {
        if (cJSON_IsString(entry)) {
            g_hash_table_insert(map, g_strdup(entry->string), g_strdup(cJSON_GetStringValue(entry)));
        } else if (cJSON_IsBool(entry)) {
            g_hash_table_insert(map, g_strdup(entry->string), g_strdup(cJSON_IsTrue(entry) ? "true" : "false"));
        } else if (cJSON_IsNumber(entry)) {
            char buf[64];
            g_snprintf(buf, sizeof(buf), "%.4f", entry->valuedouble);
            g_hash_table_insert(map, g_strdup(entry->string), g_strdup(buf));
        }
    }
    return map;
}

static const char *str_map_get(GHashTable *map, const char *key)
{
    return map && key ? g_hash_table_lookup(map, key) : NULL;
}

static void sync_alsa_audio_source_config(sbs_source_state_t *source)
{
    const char *device;

    if (!source || source->kind != SBS_SOURCE_KIND_ALSA_AUDIO) {
        return;
    }

    device = str_map_get(source->config, "device");
    source->audio.enabled = source->enabled;
    if (device && device[0]) {
        g_free(source->audio.device);
        source->audio.device = g_strdup(device);
    } else if (!source->audio.device) {
        source->audio.device = g_strdup("hw:0,2");
    }
    if (source->audio.volume < 0.0) {
        source->audio.volume = 1.0;
    }
    if (source->audio.left_gain < 0.0) {
        source->audio.left_gain = 1.0;
    }
    if (source->audio.right_gain < 0.0) {
        source->audio.right_gain = 1.0;
    }
}

static void publish_source_event(sbs_api_server_t *server, const char *topic, sbs_source_state_t *source)
{
    sbs_api_server_publish(server, topic, sbs_scene_graph_serialize_source(source));
}

static bool valid_asset_kind(const char *kind)
{
    return g_strcmp0(kind, "image") == 0 ||
           g_strcmp0(kind, "media") == 0 ||
           g_strcmp0(kind, "font") == 0;
}

static char *sanitize_filename(const char *filename)
{
    const char *base = filename ? g_path_get_basename(filename) : NULL;
    const char *src = base && base[0] ? base : "upload.bin";
    GString *out = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (g_ascii_isalnum(*p) || *p == '.' || *p == '-' || *p == '_') {
            g_string_append_c(out, (char)*p);
        } else {
            g_string_append_c(out, '_');
        }
    }
    if (out->len == 0 || g_strcmp0(out->str, ".") == 0 || g_strcmp0(out->str, "..") == 0) {
        g_string_assign(out, "upload.bin");
    }
    if (base) g_free((char *)base);
    return g_string_free(out, FALSE);
}

static char *file_uri_from_path(const char *path)
{
    char *escaped = g_uri_escape_string(path, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
    char *uri = g_strdup_printf("file://%s", escaped ? escaped : path);
    g_free(escaped);
    return uri;
}

int sbs_api_handle_source_list_kinds(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    (void)server; (void)client; (void)params; (void)error;
    const sbs_source_kind_t supported[] = {
        SBS_SOURCE_KIND_VIDEOTESTSRC,
        SBS_SOURCE_KIND_STREAMBOXSRC,
        SBS_SOURCE_KIND_V4L2SRC,
        SBS_SOURCE_KIND_URIDECODEBIN,
        SBS_SOURCE_KIND_IMAGE,
        SBS_SOURCE_KIND_TEXT,
        SBS_SOURCE_KIND_VFMCAP,
        SBS_SOURCE_KIND_ALSA_AUDIO,
    };
    cJSON *kinds = cJSON_CreateArray();

    for (uint32_t i = 0; i < G_N_ELEMENTS(supported); i++) {
        cJSON_AddItemToArray(kinds, create_source_kind_json(supported[i], false));
    }

    *result = cJSON_CreateObject();
    cJSON_AddItemToObject(*result, "kinds", kinds);
    return SBS_OK;
}

int sbs_api_handle_source_describe_kind(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error)
{
    const char *kind_id = json_str(params, "kind");
    sbs_source_kind_t kind;
    (void)server; (void)client;

    if (!sbs_scene_graph_parse_source_kind(kind_id, &kind)) {
        *error = api_error(-32004, "Unsupported source kind");
        return SBS_ERR_INVAL;
    }

    *result = cJSON_CreateObject();
    cJSON_AddItemToObject(*result, "kind", create_source_kind_json(kind, true));
    return SBS_OK;
}

int sbs_api_handle_source_discover_v4l2(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error)
{
    (void)server;
    (void)client;
    (void)params;
    (void)error;

    *result = cJSON_CreateObject();
    cJSON_AddItemToObject(*result, "devices", sbs_v4l2_discovery_list_devices());
    return SBS_OK;
}

int sbs_api_handle_source_upload_asset(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error)
{
    const char *asset_kind = json_str(params, "asset_kind");
    const char *filename = json_str(params, "filename");
    const char *data_base64 = json_str(params, "data_base64");
    const char *payload;
    const char *config_dir;
    char *safe = NULL;
    char *stored_name = NULL;
    char *asset_dir = NULL;
    char *path = NULL;
    char *uri = NULL;
    guchar *decoded = NULL;
    gsize decoded_len = 0;
    GError *gerr = NULL;
    int64_t stamp;
    (void)client;

    if (!valid_asset_kind(asset_kind)) {
        *error = api_error(-32004, "Unsupported asset kind");
        return SBS_ERR_INVAL;
    }
    if (!data_base64 || !data_base64[0]) {
        *error = api_error(-32004, "Upload data is required");
        return SBS_ERR_INVAL;
    }

    payload = strchr(data_base64, ',');
    payload = payload ? payload + 1 : data_base64;
    decoded = g_base64_decode(payload, &decoded_len);
    if (!decoded || decoded_len == 0 || decoded_len > 128u * 1024u * 1024u) {
        g_free(decoded);
        *error = api_error(-32004, "Invalid or oversized upload");
        return SBS_ERR_INVAL;
    }

    config_dir = server && server->config ? sbs_config_manager_config_dir(server->config) : NULL;
    if (!config_dir) {
        g_free(decoded);
        *error = api_error(-32006, "Upload unavailable: config manager not initialized");
        return SBS_ERR_INVAL;
    }
    safe = sanitize_filename(filename);
    stamp = g_get_real_time();
    stored_name = g_strdup_printf("%" G_GINT64_FORMAT "-%s", stamp, safe);
    asset_dir = g_build_filename(config_dir, "assets", asset_kind, NULL);
    path = g_build_filename(asset_dir, stored_name, NULL);
    if (g_mkdir_with_parents(asset_dir, 0755) != 0 ||
        !g_file_set_contents(path, (const char *)decoded, (gssize)decoded_len, &gerr)) {
        if (gerr) {
            LOG_E("failed to store uploaded %s asset: %s", asset_kind, gerr->message);
            g_error_free(gerr);
        }
        g_free(decoded);
        g_free(safe);
        g_free(stored_name);
        g_free(asset_dir);
        g_free(path);
        *error = api_error(-32005, "Unable to store uploaded asset");
        return SBS_ERR_IO;
    }

    uri = file_uri_from_path(path);
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "asset_kind", asset_kind);
    cJSON_AddStringToObject(*result, "filename", safe);
    cJSON_AddStringToObject(*result, "path", path);
    cJSON_AddStringToObject(*result, "uri", uri);
    cJSON_AddNumberToObject(*result, "size", (double)decoded_len);

    g_free(decoded);
    g_free(safe);
    g_free(stored_name);
    g_free(asset_dir);
    g_free(path);
    g_free(uri);
    return SBS_OK;
}

int sbs_api_handle_source_list(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error)
{
    GHashTableIter iter;
    gpointer key, value;
    (void)client; (void)params; (void)error;
    *result = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    g_hash_table_iter_init(&iter, server->scene_graph->sources);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(sources, key, sbs_scene_graph_serialize_source(value));
    }
    cJSON_AddItemToObject(*result, "sources", sources);
    return SBS_OK;
}

int sbs_api_handle_source_get(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    sbs_source_state_t *source;
    (void)client;
    source = sbs_scene_graph_get_source(server->scene_graph, json_str(params, "id"));
    if (!source) {
        *error = api_error(-32001, "Source not found");
        return SBS_ERR_NOT_FOUND;
    }
    *result = sbs_scene_graph_serialize_source(source);
    return SBS_OK;
}

int sbs_api_handle_source_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    const char *name = json_str(params, "name");
    const char *type = json_str(params, "type");
    cJSON *config_obj = cJSON_GetObjectItemCaseSensitive(params, "config");
    sbs_source_create_params_t create;
    sbs_source_state_t *source = NULL;
    int rc;
    (void)client;

    memset(&create, 0, sizeof(create));
    create.id = id;
    create.name = name;
    if (!sbs_scene_graph_parse_source_kind(type, &create.kind)) {
        *error = api_error(-32004, "Unsupported source kind");
        return SBS_ERR_INVAL;
    }
    create.enabled = true;

    if (create.kind == SBS_SOURCE_KIND_STREAMBOXSRC ||
        create.kind == SBS_SOURCE_KIND_V4L2SRC ||
        create.kind == SBS_SOURCE_KIND_VFMCAP) {
        create.keep_alive = true;
    } else {
        create.keep_alive = false;
    }

    if (cJSON_IsObject(config_obj)) {
        create.config = json_object_to_str_map(config_obj);
    }

    rc = sbs_scene_graph_create_source(server->scene_graph, &create, &source);
    if (create.config) {
        g_hash_table_destroy(create.config);
    }
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to create source");
        return rc;
    }
    sync_alsa_audio_source_config(source);
    publish_source_event(server, "source.created", source);

    if (create.keep_alive) {
        cJSON *start_params = cJSON_CreateObject();
        cJSON_AddStringToObject(start_params, "id", source->id);
        cJSON *start_result = NULL;
        cJSON *start_error = NULL;
        sbs_api_handle_source_start(server, NULL, start_params, &start_result, &start_error);
        cJSON_Delete(start_params);
        if (start_result) cJSON_Delete(start_result);
        if (start_error) cJSON_Delete(start_error);
    }

    *result = sbs_scene_graph_serialize_source(source);
    return SBS_OK;
}

int sbs_api_handle_source_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    const char *name = json_str(params, "name");
    cJSON *config_obj = cJSON_GetObjectItemCaseSensitive(params, "config");
    cJSON *enabled_obj = cJSON_GetObjectItemCaseSensitive(params, "enabled");
    cJSON *keep_alive_obj = cJSON_GetObjectItemCaseSensitive(params, "keep_alive");
    sbs_source_update_params_t update;
    sbs_source_state_t *source = NULL;
    int rc;
    (void)client;

    if (!id) {
        *error = api_error(-32001, "Source id is required");
        return SBS_ERR_INVAL;
    }

    memset(&update, 0, sizeof(update));
    if (name) {
        update.set_name = true;
        update.name = name;
    }
    if (enabled_obj && cJSON_IsBool(enabled_obj)) {
        update.set_enabled = true;
        update.enabled = cJSON_IsTrue(enabled_obj);
    }
    if (keep_alive_obj && cJSON_IsBool(keep_alive_obj)) {
        update.set_keep_alive = true;
        update.keep_alive = cJSON_IsTrue(keep_alive_obj);
    }
    if (cJSON_IsObject(config_obj)) {
        update.set_config = true;
        update.config = json_object_to_str_map(config_obj);
    }

    rc = sbs_scene_graph_update_source(server->scene_graph, id, &update, &source);
    if (update.config) {
        g_hash_table_destroy(update.config);
    }
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Source not found");
        return rc;
    }
    sync_alsa_audio_source_config(source);
    if (source->kind == SBS_SOURCE_KIND_ALSA_AUDIO && server->audio) {
        sbs_audio_mixer_set_source_state(server->audio, source);
    }
    publish_source_event(server, "source.updated", source);

    /* If config changed and source is running, restart the worker with new config */
    if (update.set_config && source->running && source->kind != SBS_SOURCE_KIND_ALSA_AUDIO) {
        LOG_I("source '%s' config updated while running, restarting worker", id);

        sbs_source_supervisor_stop_source(server->source_sup, id);
        if (source->slot_initialized) {
            sbs_frame_slot_flush(&source->frame_slot);
        }
        source->running = false;

        sbs_source_start_config_t cfg;
        sbs_source_start_config_fill(server->scene_graph, &server->scene_graph->canvas, source, &cfg);

        rc = sbs_source_supervisor_start_source(server->source_sup, &cfg, &source->frame_slot);
        if (rc != SBS_OK) {
            LOG_E("failed to restart source '%s' after config update: %d", id, rc);
        } else {
            source->running = true;
            g_free(source->runtime_state);
            source->runtime_state = g_strdup("starting");
            publish_source_event(server, "source.status", source);
        }
    }

    *result = sbs_scene_graph_serialize_source(source);
    return SBS_OK;
}

int sbs_api_handle_source_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_source_state_t *source;
    int rc;
    (void)client;

    if (!id) {
        *error = api_error(-32001, "Source id is required");
        return SBS_ERR_INVAL;
    }

    source = sbs_scene_graph_get_source(server->scene_graph, id);
    if (!source) {
        *error = api_error(-32001, "Source not found");
        return SBS_ERR_NOT_FOUND;
    }

    if (source->running && source->kind != SBS_SOURCE_KIND_ALSA_AUDIO) {
        sbs_source_supervisor_stop_source(server->source_sup, id);
        source->running = false;
        g_free(source->runtime_state);
        source->runtime_state = g_strdup("disabled");
        if (source->slot_initialized) {
            sbs_frame_slot_flush(&source->frame_slot);
        }
    }

    publish_source_event(server, "source.removed", source);
    rc = sbs_scene_graph_remove_source(server->scene_graph, id);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Source not found");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    source_state_free(source);
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}

int sbs_api_handle_source_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_source_state_t *source = sbs_scene_graph_get_source(server->scene_graph, id);
    sbs_source_start_config_t cfg;
    int rc;
    (void)client;

    if (!source) {
        *error = api_error(-32001, "Source not found");
        return SBS_ERR_NOT_FOUND;
    }

    sbs_source_start_config_fill(server->scene_graph, &server->scene_graph->canvas, source, &cfg);

    if (source->kind == SBS_SOURCE_KIND_ALSA_AUDIO) {
        sync_alsa_audio_source_config(source);
        if (server->audio) {
            rc = sbs_audio_mixer_set_source_state(server->audio, source);
            if (rc != SBS_OK) {
                *error = api_error(-32006, "Failed to start audio input");
                return rc;
            }
        }
        source->running = true;
        g_free(source->runtime_state);
        source->runtime_state = g_strdup("running");
        publish_source_event(server, "source.status", source);
        *result = sbs_scene_graph_serialize_source(source);
        return SBS_OK;
    }

    rc = sbs_source_supervisor_start_source(server->source_sup, &cfg, &source->frame_slot);
    if (rc != SBS_OK) {
        *error = api_error(-32006, "Failed to start source worker");
        return rc;
    }
    source->running = true;
    source->frame_width = cfg.width;
    source->frame_height = cfg.height;
    source->color_depth = 8;
    source->hdr = false;
    g_strlcpy(source->color_space, "BT.709", sizeof(source->color_space));
    g_strlcpy(source->hdr_eotf, "SDR", sizeof(source->hdr_eotf));
    g_free(source->runtime_state);
    source->runtime_state = g_strdup("starting");
    publish_source_event(server, "source.status", source);
    *result = sbs_scene_graph_serialize_source(source);
    return SBS_OK;
}

int sbs_api_handle_source_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_source_state_t *source = sbs_scene_graph_get_source(server->scene_graph, id);
    int rc;
    (void)client;
    if (!source) {
        *error = api_error(-32001, "Source not found");
        return SBS_ERR_NOT_FOUND;
    }
    if (source->kind == SBS_SOURCE_KIND_ALSA_AUDIO) {
        source->audio.enabled = false;
        if (server->audio) {
            sbs_audio_mixer_set_source_state(server->audio, source);
        }
        rc = SBS_OK;
    } else {
        rc = sbs_source_supervisor_stop_source(server->source_sup, id);
        if (rc != SBS_OK) {
            *error = api_error(-32003, "Failed to stop source");
            return rc;
        }
    }
    if (source->slot_initialized) {
        sbs_frame_slot_flush(&source->frame_slot);
    }
    source->running = false;
    g_free(source->runtime_state);
    source->runtime_state = g_strdup("disabled");
    publish_source_event(server, "source.status", source);
    *result = sbs_scene_graph_serialize_source(source);
    return SBS_OK;
}
