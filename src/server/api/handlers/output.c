#define SBS_LOG_COMP "api-output"

#include "sbs/api_server.h"
#include "sbs/config_manager.h"
#include "sbs/encoder_manager.h"
#include "sbs/log.h"

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

static const char *json_str_alias(cJSON *obj, const char *key, const char *alias)
{
    const char *value = json_str(obj, key);
    return value ? value : json_str(obj, alias);
}

static double json_num_def(cJSON *obj, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static const char *encoder_str(GHashTable *encoder, const char *key, const char *fallback)
{
    if (!encoder) return fallback;
    const char *val = g_hash_table_lookup(encoder, key);
    return val ? val : fallback;
}

static uint32_t encoder_num(GHashTable *encoder, const char *key, uint32_t fallback)
{
    if (!encoder) return fallback;
    const char *val = g_hash_table_lookup(encoder, key);
    if (!val) return fallback;
    return (uint32_t)strtoul(val, NULL, 10);
}

static const char *resolve_shared_codec_for_sink(const char *sink_type,
                                                 const char *requested_codec)
{
    (void)sink_type;
    if (requested_codec && *requested_codec)
        return requested_codec;
    return NULL;
}

static int ensure_shared_encoder_codec(sbs_api_server_t *server,
                                       const char *output_id,
                                       const char *sink_type,
                                       const char *requested_codec,
                                       const char **resolved_codec,
                                       cJSON **error)
{
    const char *codec = resolve_shared_codec_for_sink(sink_type, requested_codec);
    sbs_encoder_config_t cfg = {0};
    const char *current_codec;
    uint32_t active_branches;

    if (resolved_codec)
        *resolved_codec = codec;
    if (!server || !server->encoder_mgr || !codec)
        return SBS_OK;

    sbs_encoder_manager_get_config(server->encoder_mgr, &cfg);
    current_codec = cfg.codec ? cfg.codec : "h265";
    if (g_strcmp0(current_codec, codec) == 0)
        return SBS_OK;

    active_branches = sbs_encoder_manager_sink_count(server->encoder_mgr);
    if (active_branches > 0) {
        LOG_W("output '%s' requires codec=%s but shared encoder is codec=%s with %u active branches",
              output_id ? output_id : "<unknown>", codec, current_codec, active_branches);
        if (error)
            *error = api_error(-32006, "Output codec conflicts with active shared encoder");
        return SBS_ERR_INVAL;
    }

    LOG_I("reconfiguring shared encoder for output '%s': codec=%s -> %s",
          output_id ? output_id : "<unknown>", current_codec, codec);
    cfg.codec = codec;
    int rc = sbs_encoder_manager_update_config(server->encoder_mgr, &cfg);
    if (rc != SBS_OK && error)
        *error = api_error(-32006, "Failed to update shared encoder codec");
    return rc;
}

static void output_replace_encoder_from_json(sbs_output_state_t *output, cJSON *encoder_obj)
{
    cJSON *entry = NULL;
    cJSON *rtmp_passcode_item;
    cJSON *rtmp_stream_key_item;
    cJSON *srt_stream_key_item;
    cJSON *srt_stream_id_item;
    cJSON *srt_passphrase_item;
    char *old_rtmp_passcode = NULL;
    char *old_srt_stream_key = NULL;
    char *old_srt_passphrase = NULL;

    if (!output || !cJSON_IsObject(encoder_obj)) return;
    if (output->encoder) {
        old_rtmp_passcode = g_strdup(g_hash_table_lookup(output->encoder, "rtmp_passcode"));
        old_srt_stream_key = g_strdup(g_hash_table_lookup(output->encoder, "srt_stream_key"));
        old_srt_passphrase = g_strdup(g_hash_table_lookup(output->encoder, "srt_passphrase"));
    }
    rtmp_passcode_item = cJSON_GetObjectItemCaseSensitive(encoder_obj, "rtmp_passcode");
    rtmp_stream_key_item = cJSON_GetObjectItemCaseSensitive(encoder_obj, "rtmp_stream_key");
    srt_stream_key_item = cJSON_GetObjectItemCaseSensitive(encoder_obj, "srt_stream_key");
    srt_stream_id_item = cJSON_GetObjectItemCaseSensitive(encoder_obj, "srt_stream_id");
    srt_passphrase_item = cJSON_GetObjectItemCaseSensitive(encoder_obj, "srt_passphrase");

    if (output->encoder) g_hash_table_destroy(output->encoder);
    output->encoder = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (entry = encoder_obj->child; entry; entry = entry->next) {
        if (cJSON_IsString(entry)) {
            const char *key = entry->string;
            if (g_strcmp0(key, "rtmp_stream_key") == 0)
                key = "rtmp_passcode";
            else if (g_strcmp0(key, "srt_stream_id") == 0)
                key = "srt_stream_key";
            g_hash_table_insert(output->encoder, g_strdup(key),
                                g_strdup(cJSON_GetStringValue(entry)));
        } else if (cJSON_IsNumber(entry)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%u", (uint32_t)entry->valuedouble);
            g_hash_table_insert(output->encoder, g_strdup(entry->string), g_strdup(buf));
        }
    }
    if (!cJSON_IsString(rtmp_passcode_item) && !cJSON_IsString(rtmp_stream_key_item) &&
        old_rtmp_passcode && *old_rtmp_passcode) {
        g_hash_table_insert(output->encoder, g_strdup("rtmp_passcode"), old_rtmp_passcode);
        old_rtmp_passcode = NULL;
    }
    if (!cJSON_IsString(srt_stream_key_item) && !cJSON_IsString(srt_stream_id_item) &&
        old_srt_stream_key && *old_srt_stream_key) {
        g_hash_table_insert(output->encoder, g_strdup("srt_stream_key"), old_srt_stream_key);
        old_srt_stream_key = NULL;
    }
    if (!cJSON_IsString(srt_passphrase_item) && old_srt_passphrase && *old_srt_passphrase) {
        g_hash_table_insert(output->encoder, g_strdup("srt_passphrase"), old_srt_passphrase);
        old_srt_passphrase = NULL;
    }
    g_free(old_rtmp_passcode);
    g_free(old_srt_stream_key);
    g_free(old_srt_passphrase);
}

int sbs_api_handle_output_list(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error)
{
    GHashTableIter iter;
    gpointer key, value;
    (void)client; (void)params; (void)error;
    *result = cJSON_CreateObject();
    cJSON *outputs = cJSON_CreateObject();
    g_hash_table_iter_init(&iter, server->scene_graph->outputs);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(outputs, key, sbs_scene_graph_serialize_output_public(value));
    }
    cJSON_AddItemToObject(*result, "output_groups", outputs);
    return SBS_OK;
}

int sbs_api_handle_output_get(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    sbs_output_state_t *output = sbs_scene_graph_get_output(server->scene_graph, json_str(params, "id"));
    (void)client;
    if (!output) {
        *error = api_error(-32001, "Output not found");
        return SBS_ERR_NOT_FOUND;
    }
    *result = sbs_scene_graph_serialize_output_public(output);
    return SBS_OK;
}

int sbs_api_handle_output_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    sbs_output_create_params_t create = { json_str(params, "id"), json_str(params, "name"), true, false };
    sbs_output_state_t *output = NULL;
    int rc;
    (void)client;
    rc = sbs_scene_graph_create_output(server->scene_graph, &create, &output);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to create output");
        return rc;
    }

    output_replace_encoder_from_json(output, cJSON_GetObjectItemCaseSensitive(params, "encoder"));

    *result = sbs_scene_graph_serialize_output_public(output);
    sbs_api_server_publish(server, "output.created", sbs_scene_graph_serialize_output_public(output));
    return SBS_OK;
}

int sbs_api_handle_output_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_output_state_t *output;
    int rc;
    (void)client;

    output = sbs_scene_graph_get_output(server->scene_graph, id);
    if (output && output->running) {
        if (server->encoder_mgr && sbs_encoder_manager_has_sink(server->encoder_mgr, id)) {
            sbs_encoder_manager_remove_sink(server->encoder_mgr, id);
        } else {
            sbs_output_supervisor_stop_output(server->output_sup, id);
        }
        output->running = false;
        g_free(output->runtime_state);
        output->runtime_state = g_strdup("disabled");
    }

    rc = sbs_scene_graph_remove_output(server->scene_graph, id);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Output not found");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    sbs_config_manager_mark_dirty(server->config, server);
    return SBS_OK;
}

int sbs_api_handle_output_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_output_state_t *output = sbs_scene_graph_get_output(server->scene_graph, id);
    sbs_output_start_config_t cfg;
    int rc;
    (void)client;

    if (!output) {
        *error = api_error(-32001, "Output not found");
        return SBS_ERR_NOT_FOUND;
    }

    if (!server->comp_thread) {
        *error = api_error(-32006, "Output start unavailable: compositor thread not initialized");
        return SBS_ERR_INVAL;
    }

    /* Use encoder manager (shared encoder + tee) when available */
    if (server->encoder_mgr) {
        sbs_sink_branch_config_t sink_cfg = {0};
        const char *resolved_codec = NULL;
        sink_cfg.output_id = output->id;

        const char *sink_type = json_str(params, "sink_type");
        sink_cfg.sink_type = sink_type ? sink_type : encoder_str(output->encoder, "sink_type", "srt");

        const char *srt_uri = json_str(params, "srt_uri");
        sink_cfg.srt_uri = srt_uri ? srt_uri : encoder_str(output->encoder, "srt_uri", "srt://:8888");

        const char *srt_mode = json_str(params, "srt_mode");
        sink_cfg.srt_mode = srt_mode ? srt_mode : encoder_str(output->encoder, "srt_mode", NULL);

        const char *srt_stream_key = json_str_alias(params, "srt_stream_key", "srt_stream_id");
        sink_cfg.srt_stream_key = srt_stream_key ? srt_stream_key :
            encoder_str(output->encoder, "srt_stream_key", encoder_str(output->encoder, "srt_stream_id", NULL));

        const char *srt_passphrase = json_str(params, "srt_passphrase");
        sink_cfg.srt_passphrase = srt_passphrase ? srt_passphrase : encoder_str(output->encoder, "srt_passphrase", NULL);

        uint32_t srt_latency = (uint32_t)json_num_def(params, "srt_latency_ms", 0);
        sink_cfg.srt_latency_ms = srt_latency ? srt_latency : encoder_num(output->encoder, "srt_latency_ms", 600);

        const char *rtmp_uri = json_str(params, "rtmp_uri");
        sink_cfg.rtmp_uri = rtmp_uri ? rtmp_uri : encoder_str(output->encoder, "rtmp_uri", NULL);

        const char *rtmp_passcode = json_str_alias(params, "rtmp_stream_key", "rtmp_passcode");
        sink_cfg.rtmp_passcode = rtmp_passcode ? rtmp_passcode : encoder_str(output->encoder, "rtmp_passcode", NULL);

        const char *rtmp_flv_mode = json_str(params, "rtmp_flv_mode");
        sink_cfg.rtmp_flv_mode = rtmp_flv_mode ? rtmp_flv_mode : encoder_str(output->encoder, "rtmp_flv_mode", NULL);

        const char *file_path = json_str(params, "file_path");
        sink_cfg.file_path = file_path ? file_path : encoder_str(output->encoder, "file_path", NULL);

        const char *file_path_mode = json_str(params, "file_path_mode");
        sink_cfg.file_path_mode = file_path_mode ? file_path_mode : encoder_str(output->encoder, "file_path_mode", "file");

        const char *file_prefix = json_str(params, "file_prefix");
        sink_cfg.file_prefix = file_prefix ? file_prefix : encoder_str(output->encoder, "file_prefix", "stream");

        const char *file_container = json_str(params, "file_container");
        sink_cfg.file_container = file_container ? file_container : encoder_str(output->encoder, "file_container", "ts");

        const char *codec = json_str(params, "codec");
        rc = ensure_shared_encoder_codec(server, output->id, sink_cfg.sink_type,
            codec ? codec : encoder_str(output->encoder, "codec", NULL),
            &resolved_codec, error);
        if (rc != SBS_OK)
            return rc;

        rc = sbs_encoder_manager_add_sink(server->encoder_mgr, &sink_cfg);
        if (rc != SBS_OK) {
            *error = api_error(-32006, "Failed to add output sink branch");
            return rc;
        }

        /* Persist resolved sink config so output auto-restarts after instance restart */
        if (!output->encoder)
            output->encoder = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(output->encoder, g_strdup("sink_type"),
            g_strdup(sink_cfg.sink_type ? sink_cfg.sink_type : "srt"));
        g_hash_table_insert(output->encoder, g_strdup("srt_uri"),
            g_strdup(sink_cfg.srt_uri ? sink_cfg.srt_uri : "srt://:8888"));
        if (sink_cfg.srt_mode)
            g_hash_table_insert(output->encoder, g_strdup("srt_mode"), g_strdup(sink_cfg.srt_mode));
        if (sink_cfg.srt_stream_key)
            g_hash_table_insert(output->encoder, g_strdup("srt_stream_key"), g_strdup(sink_cfg.srt_stream_key));
        if (sink_cfg.srt_passphrase)
            g_hash_table_insert(output->encoder, g_strdup("srt_passphrase"), g_strdup(sink_cfg.srt_passphrase));
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%u", sink_cfg.srt_latency_ms);
            g_hash_table_insert(output->encoder, g_strdup("srt_latency_ms"), g_strdup(buf));
        }
        if (sink_cfg.rtmp_uri)
            g_hash_table_insert(output->encoder, g_strdup("rtmp_uri"), g_strdup(sink_cfg.rtmp_uri));
        if (sink_cfg.rtmp_passcode)
            g_hash_table_insert(output->encoder, g_strdup("rtmp_passcode"), g_strdup(sink_cfg.rtmp_passcode));
        if (sink_cfg.rtmp_flv_mode)
            g_hash_table_insert(output->encoder, g_strdup("rtmp_flv_mode"), g_strdup(sink_cfg.rtmp_flv_mode));
        if (resolved_codec && *resolved_codec)
            g_hash_table_insert(output->encoder, g_strdup("codec"), g_strdup(resolved_codec));
        if (sink_cfg.file_path)
            g_hash_table_insert(output->encoder, g_strdup("file_path"), g_strdup(sink_cfg.file_path));
        if (sink_cfg.file_path_mode)
            g_hash_table_insert(output->encoder, g_strdup("file_path_mode"), g_strdup(sink_cfg.file_path_mode));
        if (sink_cfg.file_prefix)
            g_hash_table_insert(output->encoder, g_strdup("file_prefix"), g_strdup(sink_cfg.file_prefix));
        if (sink_cfg.file_container)
            g_hash_table_insert(output->encoder, g_strdup("file_container"), g_strdup(sink_cfg.file_container));
    } else {
        /* Legacy supervisor path */
        memset(&cfg, 0, sizeof(cfg));
        cfg.output_id = output->id;
        cfg.width = server->scene_graph->canvas.width;
        cfg.height = server->scene_graph->canvas.height;
        cfg.framerate_num = server->scene_graph->canvas.fps_num;
        cfg.framerate_den = server->scene_graph->canvas.fps_den;

        const char *codec = json_str(params, "codec");
        cfg.codec = codec ? codec : encoder_str(output->encoder, "codec", "h265");

        uint32_t bitrate = (uint32_t)json_num_def(params, "bitrate_kbps", 0);
        cfg.bitrate_kbps = bitrate ? bitrate : encoder_num(output->encoder, "bitrate_kbps", 10000);

        const char *sink_type = json_str(params, "sink_type");
        cfg.sink_type = sink_type ? sink_type : encoder_str(output->encoder, "sink_type", "srt");

        const char *srt_uri = json_str(params, "srt_uri");
        cfg.srt_uri = srt_uri ? srt_uri : encoder_str(output->encoder, "srt_uri", "srt://:8888");

        const char *srt_mode = json_str(params, "srt_mode");
        cfg.srt_mode = srt_mode ? srt_mode : encoder_str(output->encoder, "srt_mode", NULL);

        const char *srt_stream_key = json_str_alias(params, "srt_stream_key", "srt_stream_id");
        cfg.srt_stream_key = srt_stream_key ? srt_stream_key :
            encoder_str(output->encoder, "srt_stream_key", encoder_str(output->encoder, "srt_stream_id", NULL));

        const char *srt_passphrase = json_str(params, "srt_passphrase");
        cfg.srt_passphrase = srt_passphrase ? srt_passphrase : encoder_str(output->encoder, "srt_passphrase", NULL);

        uint32_t srt_latency = (uint32_t)json_num_def(params, "srt_latency_ms", 0);
        cfg.srt_latency_ms = srt_latency ? srt_latency : encoder_num(output->encoder, "srt_latency_ms", 600);

        const char *rtmp_uri = json_str(params, "rtmp_uri");
        cfg.rtmp_uri = rtmp_uri ? rtmp_uri : encoder_str(output->encoder, "rtmp_uri", NULL);

        const char *rtmp_passcode = json_str_alias(params, "rtmp_stream_key", "rtmp_passcode");
        cfg.rtmp_passcode = rtmp_passcode ? rtmp_passcode : encoder_str(output->encoder, "rtmp_passcode", NULL);

        const char *file_path = json_str(params, "file_path");
        cfg.file_path = file_path ? file_path : encoder_str(output->encoder, "file_path", NULL);

        const char *file_path_mode = json_str(params, "file_path_mode");
        cfg.file_path_mode = file_path_mode ? file_path_mode : encoder_str(output->encoder, "file_path_mode", "file");

        const char *file_prefix = json_str(params, "file_prefix");
        cfg.file_prefix = file_prefix ? file_prefix : encoder_str(output->encoder, "file_prefix", "stream");

        const char *file_container = json_str(params, "file_container");
        cfg.file_container = file_container ? file_container : encoder_str(output->encoder, "file_container", "ts");

        cfg.gop_size = (uint32_t)json_num_def(params, "gop_size",
            encoder_num(output->encoder, "gop_size", cfg.framerate_num));

        rc = sbs_output_supervisor_start_output(server->output_sup, &cfg);
        if (rc != SBS_OK) {
            *error = api_error(-32006, "Failed to start output worker");
            return rc;
        }

        /* Persist resolved sink config so output auto-restarts after instance restart */
        if (!output->encoder)
            output->encoder = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_hash_table_insert(output->encoder, g_strdup("sink_type"),
            g_strdup(cfg.sink_type ? cfg.sink_type : "srt"));
        g_hash_table_insert(output->encoder, g_strdup("srt_uri"),
            g_strdup(cfg.srt_uri ? cfg.srt_uri : "srt://:8888"));
        if (cfg.srt_mode)
            g_hash_table_insert(output->encoder, g_strdup("srt_mode"), g_strdup(cfg.srt_mode));
        if (cfg.srt_stream_key)
            g_hash_table_insert(output->encoder, g_strdup("srt_stream_key"), g_strdup(cfg.srt_stream_key));
        if (cfg.srt_passphrase)
            g_hash_table_insert(output->encoder, g_strdup("srt_passphrase"), g_strdup(cfg.srt_passphrase));
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%u", cfg.srt_latency_ms);
            g_hash_table_insert(output->encoder, g_strdup("srt_latency_ms"), g_strdup(buf));
        }
        if (cfg.rtmp_uri)
            g_hash_table_insert(output->encoder, g_strdup("rtmp_uri"), g_strdup(cfg.rtmp_uri));
        if (cfg.rtmp_passcode)
            g_hash_table_insert(output->encoder, g_strdup("rtmp_passcode"), g_strdup(cfg.rtmp_passcode));
        if (cfg.file_path)
            g_hash_table_insert(output->encoder, g_strdup("file_path"), g_strdup(cfg.file_path));
        if (cfg.file_path_mode)
            g_hash_table_insert(output->encoder, g_strdup("file_path_mode"), g_strdup(cfg.file_path_mode));
        if (cfg.file_prefix)
            g_hash_table_insert(output->encoder, g_strdup("file_prefix"), g_strdup(cfg.file_prefix));
        if (cfg.file_container)
            g_hash_table_insert(output->encoder, g_strdup("file_container"), g_strdup(cfg.file_container));
    }

    output->autostart = true;
    output->running = true;
    g_free(output->runtime_state);
    output->runtime_state = g_strdup(server->encoder_mgr ? "running" : "starting");
    sbs_config_manager_mark_dirty(server->config, server);
    *result = sbs_scene_graph_serialize_output_public(output);
    sbs_api_server_publish(server, "output.status", sbs_scene_graph_serialize_output_public(output));
    return SBS_OK;
}

int sbs_api_handle_output_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_output_state_t *output = sbs_scene_graph_get_output(server->scene_graph, id);
    int rc;
    (void)client;
    if (!output) {
        *error = api_error(-32001, "Output not found");
        return SBS_ERR_NOT_FOUND;
    }
    if (server->encoder_mgr && sbs_encoder_manager_has_sink(server->encoder_mgr, id)) {
        rc = sbs_encoder_manager_remove_sink(server->encoder_mgr, id);
    } else {
        rc = sbs_output_supervisor_stop_output(server->output_sup, id);
    }
    if (rc != SBS_OK && rc != SBS_ERR_NOT_FOUND) {
        *error = api_error(-32003, "Failed to stop output");
        return rc;
    }
    output->running = false;
    output->autostart = false;
    g_free(output->runtime_state);
    output->runtime_state = g_strdup("disabled");
    sbs_config_manager_mark_dirty(server->config, server);
    *result = sbs_scene_graph_serialize_output_public(output);
    sbs_api_server_publish(server, "output.status", sbs_scene_graph_serialize_output_public(output));
    return SBS_OK;
}

int sbs_api_handle_output_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    const char *id = json_str(params, "id");
    sbs_output_state_t *output = sbs_scene_graph_get_output(server->scene_graph, id);
    bool needs_restart = false;
    (void)client;

    if (!output) {
        *error = api_error(-32001, "Output not found");
        return SBS_ERR_NOT_FOUND;
    }

    const char *name = json_str(params, "name");
    if (name) {
        g_free(output->name);
        output->name = g_strdup(name);
    }

    cJSON *encoder_obj = cJSON_GetObjectItemCaseSensitive(params, "encoder");
    if (cJSON_IsObject(encoder_obj)) {
        output_replace_encoder_from_json(output, encoder_obj);
        needs_restart = output->running;
    }

    if (needs_restart) {
        LOG_I("output '%s' encoder config updated while running, restarting", id);

        if (server->encoder_mgr && sbs_encoder_manager_has_sink(server->encoder_mgr, id)) {
            /* Remove and re-add sink branch with updated config */
            sbs_encoder_manager_remove_sink(server->encoder_mgr, id);
            output->running = false;

            sbs_sink_branch_config_t sink_cfg = {0};
            sink_cfg.output_id = output->id;
            sink_cfg.sink_type = encoder_str(output->encoder, "sink_type", "srt");
            sink_cfg.srt_uri = encoder_str(output->encoder, "srt_uri", "srt://:8888");
            sink_cfg.srt_mode = encoder_str(output->encoder, "srt_mode", NULL);
            sink_cfg.srt_stream_key = encoder_str(output->encoder, "srt_stream_key",
                                                  encoder_str(output->encoder, "srt_stream_id", NULL));
            sink_cfg.srt_passphrase = encoder_str(output->encoder, "srt_passphrase", NULL);
            sink_cfg.srt_latency_ms = encoder_num(output->encoder, "srt_latency_ms", 600);
            sink_cfg.rtmp_uri = encoder_str(output->encoder, "rtmp_uri", NULL);
            sink_cfg.rtmp_passcode = encoder_str(output->encoder, "rtmp_passcode", NULL);
            sink_cfg.rtmp_flv_mode = encoder_str(output->encoder, "rtmp_flv_mode", NULL);
            sink_cfg.file_path = encoder_str(output->encoder, "file_path", NULL);
            sink_cfg.file_path_mode = encoder_str(output->encoder, "file_path_mode", "file");
            sink_cfg.file_prefix = encoder_str(output->encoder, "file_prefix", "stream");
            sink_cfg.file_container = encoder_str(output->encoder, "file_container", "ts");

            int rc = ensure_shared_encoder_codec(server, output->id, sink_cfg.sink_type,
                encoder_str(output->encoder, "codec", NULL), NULL, NULL);
            if (rc != SBS_OK) {
                LOG_W("failed to prepare encoder for output '%s' after config update: %d", id, rc);
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("error");
                sbs_api_server_publish(server, "output.status", sbs_scene_graph_serialize_output_public(output));
                sbs_config_manager_mark_dirty(server->config, server);
                return SBS_OK;
            }

            rc = sbs_encoder_manager_add_sink(server->encoder_mgr, &sink_cfg);
            if (rc != SBS_OK) {
                LOG_W("failed to restart output '%s' after config update: %d", id, rc);
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("error");
            } else {
                output->running = true;
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("starting");
            }
        } else {
            /* Legacy supervisor path */
            sbs_output_supervisor_stop_output(server->output_sup, id);
            output->running = false;

            sbs_output_start_config_t cfg = {0};
            cfg.output_id = output->id;
            cfg.width = server->scene_graph->canvas.width;
            cfg.height = server->scene_graph->canvas.height;
            cfg.framerate_num = server->scene_graph->canvas.fps_num;
            cfg.framerate_den = server->scene_graph->canvas.fps_den;
            cfg.codec = encoder_str(output->encoder, "codec", "h265");
            cfg.bitrate_kbps = encoder_num(output->encoder, "bitrate_kbps", 10000);
            cfg.sink_type = encoder_str(output->encoder, "sink_type", "srt");
            cfg.srt_uri = encoder_str(output->encoder, "srt_uri", "srt://:8888");
            cfg.srt_mode = encoder_str(output->encoder, "srt_mode", NULL);
            cfg.srt_stream_key = encoder_str(output->encoder, "srt_stream_key",
                                             encoder_str(output->encoder, "srt_stream_id", NULL));
            cfg.srt_passphrase = encoder_str(output->encoder, "srt_passphrase", NULL);
            cfg.srt_latency_ms = encoder_num(output->encoder, "srt_latency_ms", 600);
            cfg.rtmp_uri = encoder_str(output->encoder, "rtmp_uri", NULL);
            cfg.rtmp_passcode = encoder_str(output->encoder, "rtmp_passcode", NULL);
            cfg.file_path = encoder_str(output->encoder, "file_path", NULL);
            cfg.file_path_mode = encoder_str(output->encoder, "file_path_mode", "file");
            cfg.file_prefix = encoder_str(output->encoder, "file_prefix", "stream");
            cfg.file_container = encoder_str(output->encoder, "file_container", "ts");
            cfg.gop_size = encoder_num(output->encoder, "gop_size", cfg.framerate_num);

            int rc = sbs_output_supervisor_start_output(server->output_sup, &cfg);
            if (rc != SBS_OK) {
                LOG_W("failed to restart output '%s' after encoder update: %d", id, rc);
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("error");
            } else {
                output->running = true;
                g_free(output->runtime_state);
                output->runtime_state = g_strdup("starting");
            }
        }
    }

    sbs_api_server_publish(server, "output.updated", sbs_scene_graph_serialize_output_public(output));
    *result = sbs_scene_graph_serialize_output_public(output);
    return SBS_OK;
}
