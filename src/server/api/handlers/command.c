#define SBS_LOG_COMP "api-command"

#include "sbs/api_server.h"

#include <string.h>

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static char **split_command(const char *command, gint *argc_out)
{
    gchar **tokens;
    if (!command) {
        return NULL;
    }
    tokens = g_strsplit_set(command, " \t\r\n", -1);
    *argc_out = g_strv_length(tokens);
    return tokens;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

int sbs_api_handle_command_execute(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    const char *command = json_str(params, "command");
    gchar **argv;
    gint argc = 0;
    cJSON *sub_params = NULL;
    int rc = SBS_ERR_INVAL;

    (void)client;

    if (!command || *command == '\0') {
        *error = api_error(-32602, "command is required");
        return SBS_ERR_INVAL;
    }

    argv = split_command(command, &argc);
    if (!argv || argc < 2) {
        g_strfreev(argv);
        *error = api_error(-32602, "unsupported command");
        return SBS_ERR_INVAL;
    }

    sub_params = cJSON_CreateObject();
    if (g_strcmp0(argv[0], "scene") == 0 && g_strcmp0(argv[1], "set-active") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "scene_id", argv[2]);
        if (argc >= 4) cJSON_AddStringToObject(sub_params, "transition_id", argv[3]);
        rc = sbs_api_handle_scene_set_active(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "scene") == 0 && g_strcmp0(argv[1], "set-preview") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "scene_id", argv[2]);
        rc = sbs_api_handle_scene_set_preview(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "scene") == 0 && g_strcmp0(argv[1], "transition-to-preview") == 0) {
        if (argc >= 3) cJSON_AddStringToObject(sub_params, "transition_id", argv[2]);
        rc = sbs_api_handle_scene_transition_to_preview(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "source") == 0 && g_strcmp0(argv[1], "start") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "id", argv[2]);
        rc = sbs_api_handle_source_start(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "source") == 0 && g_strcmp0(argv[1], "stop") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "id", argv[2]);
        rc = sbs_api_handle_source_stop(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "output") == 0 && g_strcmp0(argv[1], "start") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "id", argv[2]);
        rc = sbs_api_handle_output_start(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "output") == 0 && g_strcmp0(argv[1], "stop") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "id", argv[2]);
        rc = sbs_api_handle_output_stop(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "preview") == 0 && g_strcmp0(argv[1], "list-profiles") == 0) {
        rc = sbs_api_handle_preview_list_profiles(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "preview") == 0 && g_strcmp0(argv[1], "ensure") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "profile_id", argv[2]);
        rc = sbs_api_handle_preview_ensure_profile(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "preview") == 0 && g_strcmp0(argv[1], "release") == 0 && argc >= 3) {
        cJSON_AddStringToObject(sub_params, "profile_id", argv[2]);
        rc = sbs_api_handle_preview_release_profile(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "preview") == 0 && g_strcmp0(argv[1], "status") == 0) {
        if (argc >= 3) cJSON_AddStringToObject(sub_params, "profile_id", argv[2]);
        rc = sbs_api_handle_preview_get_status(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "system") == 0 && g_strcmp0(argv[1], "get-state") == 0) {
        rc = sbs_api_handle_system_get_state(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "system") == 0 && g_strcmp0(argv[1], "get-info") == 0) {
        rc = sbs_api_handle_system_get_info(server, client, sub_params, result, error);
    } else if (g_strcmp0(argv[0], "snapshot") == 0 && g_strcmp0(argv[1], "capture") == 0) {
        if (argc >= 3) cJSON_AddStringToObject(sub_params, "format", argv[2]);
        rc = sbs_api_handle_snapshot_capture(server, client, sub_params, result, error);
    } else {
        *error = api_error(-32602, "unsupported command");
    }

    cJSON_Delete(sub_params);
    g_strfreev(argv);
    return rc;
}
