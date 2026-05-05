#define SBS_LOG_COMP "api-instance"

#include "sbs/api_server.h"

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static cJSON *json_dup_or_null(cJSON *item)
{
    char *json;
    cJSON *copy;
    if (!item) {
        return cJSON_CreateNull();
    }
    json = cJSON_PrintUnformatted(item);
    if (!json) {
        return cJSON_CreateNull();
    }
    copy = cJSON_Parse(json);
    g_free(json);
    return copy ? copy : cJSON_CreateNull();
}

static uint32_t json_instance_id(cJSON *params, bool *ok)
{
    cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, "instance_id") : NULL;
    if (ok) *ok = false;
    if (!cJSON_IsNumber(item) || item->valueint < 0) {
        return 0;
    }
    if (ok) *ok = true;
    return (uint32_t)item->valueint;
}

static const char *json_string_field(cJSON *params, const char *key)
{
    cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

static cJSON *forwarded_params_with_peer_ip(cJSON *params, const char *method, const char *peer_ip)
{
    cJSON *forwarded = json_dup_or_null(params);

    if (!peer_ip || !*peer_ip || !method ||
        (strcmp(method, "preview.webrtc.answer") != 0 &&
         strcmp(method, "preview.webrtc.ice") != 0)) {
        return forwarded;
    }

    if (!cJSON_IsObject(forwarded)) {
        cJSON_Delete(forwarded);
        forwarded = cJSON_CreateObject();
    }
    cJSON_AddStringToObject(forwarded, "__client_peer_ip", peer_ip);
    return forwarded;
}

static int require_manager(sbs_api_server_t *server, cJSON **error)
{
    if (!server || !server->instance_mgr) {
        if (error) {
            *error = api_error(-32603, "Instance manager unavailable");
        }
        return SBS_ERR_INVAL;
    }
    return SBS_OK;
}

int sbs_api_handle_instance_list(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    (void)params;
    if (!server->instance_mgr) {
        cJSON *instances = cJSON_CreateArray();
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "instance_id", (double)server->instance_id);
        cJSON_AddStringToObject(entry, "name", "Local");
        cJSON_AddBoolToObject(entry, "desired_running", true);
        cJSON_AddBoolToObject(entry, "running", server->running);
        cJSON_AddNumberToObject(entry, "pid", (double)getpid());
        cJSON_AddNumberToObject(entry, "api_port", (double)server->port);
        cJSON_AddNumberToObject(entry, "preview_port", (double)server->preview_port);
        cJSON_AddItemToArray(instances, entry);
        *result = cJSON_CreateObject();
        cJSON_AddItemToObject(*result, "instances", instances);
        return SBS_OK;
    }
    *result = sbs_instance_manager_build_inventory(server->instance_mgr);
    return *result ? SBS_OK : SBS_ERR_NOMEM;
}

int sbs_api_handle_instance_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    uint32_t instance_id = 0;
    sbs_instance_info_t info;
    const char *name;
    int rc;
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    name = json_string_field(params, "name");
    rc = sbs_instance_manager_create_instance(server->instance_mgr, name, &instance_id);
    if (rc == SBS_ERR_INVAL) {
        *error = api_error(-32602, "Instance name already exists or is invalid");
        return rc;
    }
    if (rc != SBS_OK || sbs_instance_manager_get_info(server->instance_mgr, instance_id, &info) != SBS_OK) {
        *error = api_error(-32003, "Unable to create instance");
        return rc == SBS_OK ? SBS_ERR_IO : rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*result, "instance_id", (double)info.instance_id);
    cJSON_AddStringToObject(*result, "name", info.name ? info.name : "");
    cJSON_AddBoolToObject(*result, "running", info.running);
    cJSON_AddBoolToObject(*result, "enabled", info.desired_running);
    cJSON_AddNumberToObject(*result, "api_port", (double)info.api_port);
    cJSON_AddNumberToObject(*result, "preview_port", (double)info.preview_port);
    return SBS_OK;
}

static int build_instance_info_result(sbs_api_server_t *server, uint32_t instance_id, cJSON **result, cJSON **error)
{
    sbs_instance_info_t info;
    if (sbs_instance_manager_get_info(server->instance_mgr, instance_id, &info) != SBS_OK) {
        *error = api_error(-32001, "Instance not found");
        return SBS_ERR_NOT_FOUND;
    }
    *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*result, "instance_id", (double)info.instance_id);
    cJSON_AddStringToObject(*result, "name", info.name ? info.name : "");
    cJSON_AddBoolToObject(*result, "enabled", info.desired_running);
    cJSON_AddBoolToObject(*result, "running", info.running);
    cJSON_AddNumberToObject(*result, "pid", (double)info.pid);
    cJSON_AddNumberToObject(*result, "api_port", (double)info.api_port);
    cJSON_AddNumberToObject(*result, "preview_port", (double)info.preview_port);
    return SBS_OK;
}

int sbs_api_handle_instance_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    bool ok = false;
    uint32_t instance_id;
    const char *name;
    cJSON *enabled_item;
    bool enabled_value;
    bool *enabled_ptr = NULL;
    int rc;
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    name = json_string_field(params, "name");
    enabled_item = params ? cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    if (cJSON_IsBool(enabled_item)) {
        enabled_value = cJSON_IsTrue(enabled_item);
        enabled_ptr = &enabled_value;
    }
    if (!name && !enabled_ptr) {
        *error = api_error(-32602, "name or enabled is required");
        return SBS_ERR_INVAL;
    }
    rc = sbs_instance_manager_update_instance(server->instance_mgr, instance_id, name, enabled_ptr);
    if (rc == SBS_ERR_INVAL) {
        *error = api_error(-32602, "Instance name already exists or is invalid");
        return rc;
    }
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to update instance");
        return rc;
    }
    return build_instance_info_result(server, instance_id, result, error);
}

static int instance_toggle_common(sbs_api_server_t *server,
                                  cJSON *params,
                                  cJSON **result,
                                  cJSON **error,
                                  bool enabled)
{
    bool ok = false;
    uint32_t instance_id;
    int rc;
    (void)params;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    rc = sbs_instance_manager_update_instance(server->instance_mgr, instance_id, NULL, &enabled);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK) {
        *error = api_error(-32003, enabled ? "Unable to enable instance" : "Unable to disable instance");
        return rc;
    }
    return build_instance_info_result(server, instance_id, result, error);
}

int sbs_api_handle_instance_enable(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    return instance_toggle_common(server, params, result, error, true);
}

int sbs_api_handle_instance_disable(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    return instance_toggle_common(server, params, result, error, false);
}

static int instance_lifecycle_common(sbs_api_server_t *server,
                                     cJSON *params,
                                     cJSON **result,
                                     cJSON **error,
                                     bool start)
{
    bool ok = false;
    uint32_t instance_id;
    int rc;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    rc = start ? sbs_instance_manager_start_instance(server->instance_mgr, instance_id)
               : sbs_instance_manager_stop_instance(server->instance_mgr, instance_id);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK) {
        *error = api_error(-32003, start ? "Unable to start instance" : "Unable to stop instance");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    cJSON_AddNumberToObject(*result, "instance_id", (double)instance_id);
    return SBS_OK;
}

int sbs_api_handle_instance_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    return instance_lifecycle_common(server, params, result, error, true);
}

int sbs_api_handle_instance_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    return instance_lifecycle_common(server, params, result, error, false);
}

int sbs_api_handle_instance_restart(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    bool ok = false;
    uint32_t instance_id;
    int rc;
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    rc = sbs_instance_manager_stop_instance(server->instance_mgr, instance_id);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to stop instance for restart");
        return rc;
    }
    rc = sbs_instance_manager_start_instance(server->instance_mgr, instance_id);
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to start instance after restart");
        return rc;
    }
    return build_instance_info_result(server, instance_id, result, error);
}

int sbs_api_handle_instance_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    bool ok = false;
    uint32_t instance_id;
    int rc;
    (void)client;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    rc = sbs_instance_manager_remove_instance(server->instance_mgr, instance_id);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to remove instance");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}

static int instance_rpc_passthrough(sbs_api_server_t *server,
                                    cJSON *params,
                                    const char *method,
                                    cJSON **result,
                                    cJSON **error)
{
    bool ok = false;
    uint32_t instance_id;
    cJSON *request;
    char *request_json;
    char *response_json = NULL;
    cJSON *response;
    cJSON *payload;
    int rc;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(request, "id", 1);
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "params", cJSON_CreateObject());
    request_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    rc = sbs_instance_manager_call_json(server->instance_mgr, instance_id, request_json, &response_json);
    g_free(request_json);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK || !response_json) {
        *error = api_error(-32003, "Instance unavailable");
        return rc == SBS_OK ? SBS_ERR_IO : rc;
    }
    response = cJSON_Parse(response_json);
    g_free(response_json);
    if (!cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        *error = api_error(-32603, "Invalid instance response");
        return SBS_ERR_IO;
    }
    payload = cJSON_GetObjectItemCaseSensitive(response, "result");
    if (payload) {
        *result = json_dup_or_null(payload);
        cJSON_Delete(response);
        return SBS_OK;
    }
    payload = cJSON_GetObjectItemCaseSensitive(response, "error");
    *error = payload ? json_dup_or_null(payload) : api_error(-32603, "Invalid instance response");
    cJSON_Delete(response);
    return SBS_ERR_IO;
}

int sbs_api_handle_instance_get_info(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    return instance_rpc_passthrough(server, params, "system.getInfo", result, error);
}

int sbs_api_handle_instance_get_state(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    return instance_rpc_passthrough(server, params, "system.getState", result, error);
}

int sbs_api_handle_instance_call(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    bool ok = false;
    uint32_t instance_id;
    cJSON *method_item;
    cJSON *request;
    cJSON *forwarded_params;
    char *request_json;
    char *response_json = NULL;
    cJSON *response;
    cJSON *payload;
    int rc;
    if (require_manager(server, error) != SBS_OK) return SBS_ERR_INVAL;
    instance_id = json_instance_id(params, &ok);
    if (!ok) {
        *error = api_error(-32602, "instance_id is required");
        return SBS_ERR_INVAL;
    }
    method_item = cJSON_GetObjectItemCaseSensitive(params, "method");
    if (!cJSON_IsString(method_item)) {
        *error = api_error(-32602, "method is required");
        return SBS_ERR_INVAL;
    }
    request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(request, "id", 1);
    cJSON_AddStringToObject(request, "method", cJSON_GetStringValue(method_item));
    forwarded_params = forwarded_params_with_peer_ip(cJSON_GetObjectItemCaseSensitive(params, "params"),
                                                     cJSON_GetStringValue(method_item),
                                                     client ? client->peer_ip : NULL);
    cJSON_AddItemToObject(request, "params", forwarded_params);
    request_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    rc = sbs_instance_manager_call_json(server->instance_mgr, instance_id, request_json, &response_json);
    g_free(request_json);
    if (rc == SBS_ERR_NOT_FOUND) {
        *error = api_error(-32001, "Instance not found");
        return rc;
    }
    if (rc != SBS_OK || !response_json) {
        *error = api_error(-32003, "Instance unavailable");
        return rc == SBS_OK ? SBS_ERR_IO : rc;
    }
    response = cJSON_Parse(response_json);
    g_free(response_json);
    if (!cJSON_IsObject(response)) {
        if (response) cJSON_Delete(response);
        *error = api_error(-32603, "Invalid instance response");
        return SBS_ERR_IO;
    }
    payload = cJSON_GetObjectItemCaseSensitive(response, "result");
    if (payload) {
        *result = json_dup_or_null(payload);
        cJSON_Delete(response);
        return SBS_OK;
    }
    payload = cJSON_GetObjectItemCaseSensitive(response, "error");
    *error = payload ? json_dup_or_null(payload) : api_error(-32603, "Invalid instance response");
    cJSON_Delete(response);
    return SBS_ERR_IO;
}
