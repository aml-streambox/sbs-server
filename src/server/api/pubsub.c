#define SBS_LOG_COMP "api-pubsub"

#include "sbs/api_server.h"

#include <string.h>

static gboolean topic_matches_pattern(const char *topic, const char *pattern)
{
    const char *star;
    size_t prefix_len;

    if (!topic || !pattern) {
        return FALSE;
    }
    if (strcmp(pattern, "*") == 0) {
        return TRUE;
    }
    star = strchr(pattern, '*');
    if (!star) {
        return strcmp(topic, pattern) == 0;
    }
    prefix_len = (size_t)(star - pattern);
    return strncmp(topic, pattern, prefix_len) == 0;
}

bool sbs_api_client_matches_topic(const sbs_api_client_t *client,
                                  const char *topic)
{
    guint i;

    if (!client || !topic) {
        return false;
    }
    for (i = 0; i < client->subscriptions->len; i++) {
        const char *pattern = g_ptr_array_index(client->subscriptions, i);
        if (topic_matches_pattern(topic, pattern)) {
            return true;
        }
    }
    return false;
}

char *sbs_api_client_take_message(sbs_api_client_t *client)
{
    if (!client || client->outbox->len == 0) {
        return NULL;
    }
    return g_ptr_array_steal_index(client->outbox, 0);
}

void sbs_api_server_publish(sbs_api_server_t *server,
                            const char *topic,
                            cJSON *data)
{
    GHashTableIter iter;
    gpointer key, value;
    cJSON *msg = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    char *json;

    if (!server || !topic || !data) {
        return;
    }

    if (server->config &&
        !g_str_has_prefix(topic, "telemetry.") &&
        !g_str_has_prefix(topic, "audio.level") &&
        !g_str_has_prefix(topic, "preview.profile.")) {
        sbs_config_manager_mark_dirty(server->config, server);
    }

    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddStringToObject(msg, "method", "pubsub.event");
    cJSON_AddStringToObject(params, "topic", topic);
    cJSON_AddItemToObject(params, "data", data);
    cJSON_AddItemToObject(msg, "params", params);
    json = cJSON_PrintUnformatted(msg);

    g_hash_table_iter_init(&iter, server->clients);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_api_client_t *client = value;
        if (sbs_api_client_matches_topic(client, topic)) {
            g_ptr_array_add(client->outbox, g_strdup(json));
        }
    }

    free(json);
    cJSON_Delete(msg);
}
