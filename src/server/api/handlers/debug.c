#define SBS_LOG_COMP "api-debug"

#include "sbs/api_server.h"

#define DEBUG_LOG_DEFAULT_LINES 260
#define DEBUG_LOG_MAX_LINES 1000

static int json_int_clamped(cJSON *obj, const char *key, int fallback, int min, int max)
{
    cJSON *item = cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    int value;

    if (!cJSON_IsNumber(item)) {
        return fallback;
    }

    value = (int)item->valuedouble;
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

int sbs_api_handle_debug_get_logs(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error)
{
    char lines_arg[32];
    char *stdout_buf = NULL;
    char *stderr_buf = NULL;
    GError *spawn_error = NULL;
    GError *status_error = NULL;
    gint status = 0;
    int lines;
    gboolean ok;
    gchar *argv[9];

    (void)server;
    (void)client;
    (void)error;

    lines = json_int_clamped(params, "lines", DEBUG_LOG_DEFAULT_LINES, 1, DEBUG_LOG_MAX_LINES);
    g_snprintf(lines_arg, sizeof(lines_arg), "%d", lines);

    argv[0] = "journalctl";
    argv[1] = "-u";
    argv[2] = "sbs-server.service";
    argv[3] = "--no-pager";
    argv[4] = "-n";
    argv[5] = lines_arg;
    argv[6] = "-o";
    argv[7] = "short-iso";
    argv[8] = NULL;

    /* Keep this as a fixed argv vector; the line count is clamped above and
     * no shell is involved. */
    ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, &stdout_buf, &stderr_buf, &status, &spawn_error);

    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "source", "journalctl");
    cJSON_AddStringToObject(*result, "unit", "sbs-server.service");
    cJSON_AddNumberToObject(*result, "lines", lines);

    if (!ok) {
        cJSON_AddBoolToObject(*result, "ok", false);
        cJSON_AddStringToObject(*result, "logs", "");
        cJSON_AddStringToObject(*result, "error", spawn_error ? spawn_error->message : "failed to run journalctl");
        if (spawn_error) g_error_free(spawn_error);
        g_free(stdout_buf);
        g_free(stderr_buf);
        return SBS_OK;
    }

    if (!g_spawn_check_wait_status(status, &status_error)) {
        cJSON_AddBoolToObject(*result, "ok", false);
        cJSON_AddStringToObject(*result, "error", status_error ? status_error->message : "journalctl failed");
        if (status_error) g_error_free(status_error);
    } else {
        cJSON_AddBoolToObject(*result, "ok", true);
    }

    cJSON_AddStringToObject(*result, "logs", stdout_buf ? stdout_buf : "");
    if (stderr_buf && *stderr_buf) {
        cJSON_AddStringToObject(*result, "stderr", stderr_buf);
    }

    g_free(stdout_buf);
    g_free(stderr_buf);
    return SBS_OK;
}
