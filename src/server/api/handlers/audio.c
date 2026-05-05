#define SBS_LOG_COMP "api-audio"

#include "sbs/api_server.h"

#include <stdlib.h>

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

static double json_num(cJSON *obj, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool json_bool(cJSON *obj, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;
    return fallback;
}

static cJSON *json_clone(cJSON *item)
{
    char *payload = cJSON_PrintUnformatted(item);
    cJSON *clone = payload ? cJSON_Parse(payload) : NULL;
    free(payload);
    return clone ? clone : cJSON_CreateNull();
}

int sbs_api_handle_audio_get_levels(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    (void)client; (void)params; (void)error;
    *result = sbs_audio_mixer_serialize_state(server->audio);
    return SBS_OK;
}

int sbs_api_handle_audio_set_source(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    sbs_source_state_t *source;
    int rc;
    (void)client;
    source = sbs_scene_graph_get_source(server->scene_graph, json_str(params, "source_id"));
    if (!source) {
        *error = api_error(-32001, "Source not found");
        return SBS_ERR_NOT_FOUND;
    }
    source->audio.enabled = json_bool(params, "enabled", true);
    source->audio.volume = json_num(params, "volume", source->audio.volume > 0 ? source->audio.volume : 1.0);
    source->audio.mute = json_bool(params, "mute", source->audio.mute);
    source->audio.monitor = json_bool(params, "monitor", source->audio.monitor);
    if (json_str(params, "device")) {
        g_free(source->audio.device);
        source->audio.device = g_strdup(json_str(params, "device"));
    }
    if (source->audio.enabled && !source->audio.device) {
        source->audio.device = g_strdup("hw:0,2");
    }
    rc = sbs_audio_mixer_set_source_state(server->audio, source);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to update source audio state");
        return rc;
    }
    *result = sbs_scene_graph_serialize_source(source);
    sbs_api_server_publish(server, "audio.source.updated", json_clone(*result));
    return SBS_OK;
}

int sbs_api_handle_audio_set_scene_item(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_item_state_t *item;
    sbs_audio_binding_t audio = {0};
    int rc;
    (void)client;

    item = sbs_scene_graph_get_item(server->scene_graph,
                                    json_str(params, "scene_id"),
                                    json_str(params, "item_id"));
    if (!item) {
        *error = api_error(-32001, "Scene item not found");
        return SBS_ERR_NOT_FOUND;
    }

    audio.enabled = json_bool(params, "enabled", item->audio.enabled);
    audio.volume = json_num(params, "volume", item->audio.volume > 0 ? item->audio.volume : 1.0);
    audio.mute = json_bool(params, "mute", item->audio.mute);
    audio.monitor = json_bool(params, "monitor", item->audio.monitor);
    audio.device = g_strdup(json_str(params, "device") ? json_str(params, "device") :
                            (item->audio.device ? item->audio.device : "hw:0,2"));
    if (audio.enabled && !audio.device) {
        audio.device = g_strdup("hw:0,2");
    }

    rc = sbs_scene_graph_update_item_audio(server->scene_graph,
                                           json_str(params, "scene_id"),
                                           json_str(params, "item_id"),
                                           &audio,
                                           &item);
    g_free(audio.device);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to update scene item audio state");
        return rc;
    }

    rc = sbs_audio_mixer_set_scene_item_state(server->audio, item);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to update mixer scene item audio state");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    *result = sbs_scene_graph_serialize_scene(sbs_scene_graph_get_scene(server->scene_graph,
                                                                        json_str(params, "scene_id")));
    sbs_api_server_publish(server, "scene.changed", json_clone(*result));
    return SBS_OK;
}

int sbs_api_handle_audio_set_master(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_audio_mixer_set_master(server->audio,
                                    json_num(params, "volume", 1.0),
                                    json_bool(params, "mute", false));
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to update master audio state");
        return rc;
    }
    *result = sbs_audio_mixer_serialize_state(server->audio);
    sbs_api_server_publish(server, "audio.master.updated", json_clone(*result));
    return SBS_OK;
}
