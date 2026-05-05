#define SBS_LOG_COMP "api-snapshot"

#include "sbs/api_server.h"

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

int sbs_api_handle_snapshot_capture(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    char *json;
    cJSON *event_payload;
    (void)client;
    rc = sbs_snapshot_engine_capture(server->snapshot, json_str(params, "format"), result);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to capture snapshot");
        return rc;
    }
    json = cJSON_PrintUnformatted(*result);
    event_payload = json ? cJSON_Parse(json) : NULL;
    if (json) {
        free(json);
    }
    if (event_payload) {
        sbs_api_server_publish(server, "snapshot.captured", event_payload);
    }
    return SBS_OK;
}
