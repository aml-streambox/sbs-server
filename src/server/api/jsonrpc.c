#define SBS_LOG_COMP "api-jsonrpc"

#include "sbs/api_server.h"

#include <string.h>

static cJSON *rpc_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static void add_id_field(cJSON *response, cJSON *id)
{
    if (!id || cJSON_IsNull(id)) {
        cJSON_AddNullToObject(response, "id");
    } else if (cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(response, "id", id->valuedouble);
    } else if (cJSON_IsString(id)) {
        cJSON_AddStringToObject(response, "id", cJSON_GetStringValue(id));
    } else {
        cJSON_AddNullToObject(response, "id");
    }
}

static void register_method(sbs_api_server_t *server,
                            const char *name,
                            sbs_rpc_handler_t handler)
{
    g_hash_table_insert(server->methods, (gpointer)name, handler);
}

void sbs_api_register_core_methods(sbs_api_server_t *server)
{
    register_method(server, "system.getState", sbs_api_handle_system_get_state);
    register_method(server, "system.getInfo", sbs_api_handle_system_get_info);
    register_method(server, "source.list", sbs_api_handle_source_list);
    register_method(server, "source.get", sbs_api_handle_source_get);
    register_method(server, "source.listKinds", sbs_api_handle_source_list_kinds);
    register_method(server, "source.describeKind", sbs_api_handle_source_describe_kind);
    register_method(server, "source.discoverV4L2", sbs_api_handle_source_discover_v4l2);
    register_method(server, "source.uploadAsset", sbs_api_handle_source_upload_asset);
    register_method(server, "source.create", sbs_api_handle_source_create);
    register_method(server, "source.update", sbs_api_handle_source_update);
    register_method(server, "source.remove", sbs_api_handle_source_remove);
    register_method(server, "source.start", sbs_api_handle_source_start);
    register_method(server, "source.stop", sbs_api_handle_source_stop);
    register_method(server, "scene.list", sbs_api_handle_scene_list);
    register_method(server, "scene.get", sbs_api_handle_scene_get);
    register_method(server, "scene.create", sbs_api_handle_scene_create);
    register_method(server, "scene.update", sbs_api_handle_scene_update);
    register_method(server, "scene.remove", sbs_api_handle_scene_remove);
    register_method(server, "scene.setActive", sbs_api_handle_scene_set_active);
    register_method(server, "scene.setPreview", sbs_api_handle_scene_set_preview);
    register_method(server, "scene.transitionToPreview", sbs_api_handle_scene_transition_to_preview);
    register_method(server, "scene.transition.update", sbs_api_handle_scene_transition_update);
    register_method(server, "scene.item.add", sbs_api_handle_scene_item_add);
    register_method(server, "scene.item.update", sbs_api_handle_scene_item_update);
    register_method(server, "scene.item.remove", sbs_api_handle_scene_item_remove);
    register_method(server, "scene.item.reorder", sbs_api_handle_scene_item_reorder);
    register_method(server, "filter.add", sbs_api_handle_filter_add);
    register_method(server, "filter.update", sbs_api_handle_filter_update);
    register_method(server, "filter.remove", sbs_api_handle_filter_remove);
    register_method(server, "output.list", sbs_api_handle_output_list);
    register_method(server, "output.get", sbs_api_handle_output_get);
    register_method(server, "output.create", sbs_api_handle_output_create);
    register_method(server, "output.remove", sbs_api_handle_output_remove);
    register_method(server, "output.start", sbs_api_handle_output_start);
    register_method(server, "output.stop", sbs_api_handle_output_stop);
    register_method(server, "output.update", sbs_api_handle_output_update);
    register_method(server, "encoder.getConfig", sbs_api_handle_encoder_get_config);
    register_method(server, "encoder.updateConfig", sbs_api_handle_encoder_update_config);
    register_method(server, "canvas.update", sbs_api_handle_canvas_update);
    register_method(server, "canvas.apply", sbs_api_handle_canvas_apply);
    register_method(server, "audio.getLevels", sbs_api_handle_audio_get_levels);
    register_method(server, "audio.setSource", sbs_api_handle_audio_set_source);
    register_method(server, "audio.setSceneItem", sbs_api_handle_audio_set_scene_item);
    register_method(server, "audio.setMaster", sbs_api_handle_audio_set_master);
    register_method(server, "config.export", sbs_api_handle_config_export);
    register_method(server, "config.import", sbs_api_handle_config_import);
    register_method(server, "config.reset", sbs_api_handle_config_reset);
    register_method(server, "snapshot.capture", sbs_api_handle_snapshot_capture);
    register_method(server, "preview.listProfiles", sbs_api_handle_preview_list_profiles);
    register_method(server, "preview.ensureProfile", sbs_api_handle_preview_ensure_profile);
    register_method(server, "preview.getStatus", sbs_api_handle_preview_get_status);
    register_method(server, "preview.releaseProfile", sbs_api_handle_preview_release_profile);
    register_method(server, "preview.webrtc.start", sbs_api_handle_preview_webrtc_start);
    register_method(server, "preview.webrtc.answer", sbs_api_handle_preview_webrtc_answer);
    register_method(server, "preview.webrtc.ice", sbs_api_handle_preview_webrtc_ice);
    register_method(server, "preview.getEncoderConfig", sbs_api_handle_preview_get_encoder_config);
    register_method(server, "preview.updateEncoderConfig", sbs_api_handle_preview_update_encoder_config);
    register_method(server, "command.execute", sbs_api_handle_command_execute);
    register_method(server, "pubsub.subscribe", sbs_api_handle_pubsub_subscribe);
    register_method(server, "pubsub.unsubscribe", sbs_api_handle_pubsub_unsubscribe);
    register_method(server, "instance.list", sbs_api_handle_instance_list);
}

void sbs_api_register_controller_methods(sbs_api_server_t *server)
{
    register_method(server, "instance.list", sbs_api_handle_instance_list);
    register_method(server, "instance.create", sbs_api_handle_instance_create);
    register_method(server, "instance.update", sbs_api_handle_instance_update);
    register_method(server, "instance.enable", sbs_api_handle_instance_enable);
    register_method(server, "instance.disable", sbs_api_handle_instance_disable);
    register_method(server, "instance.start", sbs_api_handle_instance_start);
    register_method(server, "instance.stop", sbs_api_handle_instance_stop);
    register_method(server, "instance.restart", sbs_api_handle_instance_restart);
    register_method(server, "instance.remove", sbs_api_handle_instance_remove);
    register_method(server, "instance.getInfo", sbs_api_handle_instance_get_info);
    register_method(server, "instance.getState", sbs_api_handle_instance_get_state);
    register_method(server, "instance.call", sbs_api_handle_instance_call);
}

void sbs_api_server_configure_controller(sbs_api_server_t *server,
                                         sbs_instance_manager_t *instance_mgr)
{
    if (!server) {
        return;
    }
    server->instance_mgr = instance_mgr;
    g_hash_table_remove_all(server->methods);
    sbs_api_register_controller_methods(server);
}

void sbs_api_server_set_instance_id(sbs_api_server_t *server, uint32_t instance_id)
{
    if (server) {
        server->instance_id = instance_id;
    }
}

int sbs_api_server_dispatch_json(sbs_api_server_t *server,
                                 sbs_api_client_t *client,
                                 const char *request_json,
                                 char **response_json)
{
    cJSON *request;
    cJSON *response;
    cJSON *params;
    cJSON *id;
    cJSON *method_item;
    cJSON *result = NULL;
    cJSON *error = NULL;
    sbs_rpc_handler_t handler;
    int rc;

    if (!server || !request_json || !response_json) {
        return SBS_ERR_INVAL;
    }

    request = cJSON_Parse(request_json);
    if (!request || !cJSON_IsObject(request)) {
        if (request) cJSON_Delete(request);
        response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "jsonrpc", "2.0");
        cJSON_AddNullToObject(response, "id");
        cJSON_AddItemToObject(response, "error", rpc_error(-32700, "Parse error"));
        *response_json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        return SBS_OK;
    }

    id = cJSON_GetObjectItemCaseSensitive(request, "id");
    params = cJSON_GetObjectItemCaseSensitive(request, "params");
    method_item = cJSON_GetObjectItemCaseSensitive(request, "method");

    response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    add_id_field(response, id);

    if (!cJSON_IsString(method_item)) {
        cJSON_AddItemToObject(response, "error", rpc_error(-32600, "Invalid Request"));
        *response_json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        cJSON_Delete(request);
        return SBS_OK;
    }

    handler = g_hash_table_lookup(server->methods, cJSON_GetStringValue(method_item));
    if (!handler) {
        cJSON_AddItemToObject(response, "error", rpc_error(-32601, "Method not found"));
        *response_json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        cJSON_Delete(request);
        return SBS_OK;
    }

    rc = handler(server, client, params, &result, &error);
    if (rc == SBS_OK && result) {
        cJSON_AddItemToObject(response, "result", result);
    } else if (error) {
        cJSON_AddItemToObject(response, "error", error);
    } else {
        cJSON_AddItemToObject(response, "error", rpc_error(-32603, "Internal error"));
    }

    *response_json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    cJSON_Delete(request);
    return SBS_OK;
}
