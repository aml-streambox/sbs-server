#define SBS_LOG_COMP "api-pubsub-handler"

#include "sbs/api_server.h"

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

int sbs_api_handle_pubsub_subscribe(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *topics = cJSON_GetObjectItemCaseSensitive(params, "topics");
    int i;
    (void)server;

    if (!client || !cJSON_IsArray(topics)) {
        *error = api_error(-32602, "topics must be an array");
        return SBS_ERR_INVAL;
    }
    *result = cJSON_CreateObject();
    cJSON *subscribed = cJSON_CreateArray();
    for (i = 0; i < cJSON_GetArraySize(topics); i++) {
        const char *topic = cJSON_GetStringValue(cJSON_GetArrayItem(topics, i));
        if (topic) {
            g_ptr_array_add(client->subscriptions, g_strdup(topic));
            cJSON_AddItemToArray(subscribed, cJSON_CreateString(topic));
        }
    }
    cJSON_AddItemToObject(*result, "subscribed", subscribed);
    return SBS_OK;
}

int sbs_api_handle_pubsub_unsubscribe(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *topics = cJSON_GetObjectItemCaseSensitive(params, "topics");
    int i;
    (void)server;

    if (!client || !cJSON_IsArray(topics)) {
        *error = api_error(-32602, "topics must be an array");
        return SBS_ERR_INVAL;
    }
    *result = cJSON_CreateObject();
    cJSON *unsubscribed = cJSON_CreateArray();
    for (i = 0; i < cJSON_GetArraySize(topics); i++) {
        const char *topic = cJSON_GetStringValue(cJSON_GetArrayItem(topics, i));
        guint j = 0;
        while (topic && j < client->subscriptions->len) {
            const char *sub = g_ptr_array_index(client->subscriptions, j);
            if (strcmp(topic, sub) == 0) {
                g_ptr_array_remove_index(client->subscriptions, j);
                cJSON_AddItemToArray(unsubscribed, cJSON_CreateString(topic));
                break;
            }
            j++;
        }
    }
    cJSON_AddItemToObject(*result, "unsubscribed", unsubscribed);
    return SBS_OK;
}
