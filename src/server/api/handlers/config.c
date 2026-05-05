#define SBS_LOG_COMP "api-config"

#include "sbs/api_server.h"

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

int sbs_api_handle_config_export(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    (void)client; (void)params; (void)error;
    *result = sbs_config_manager_build_bundle(server);
    return SBS_OK;
}

int sbs_api_handle_config_import(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    if (!cJSON_IsObject(params)) {
        *error = api_error(-32602, "config bundle object required");
        return SBS_ERR_INVAL;
    }
    rc = sbs_config_manager_apply_bundle(server->config, server, params);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to import config bundle");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}

int sbs_api_handle_config_reset(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client; (void)params;
    rc = sbs_config_manager_reset(server->config, server);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to reset configuration");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}
