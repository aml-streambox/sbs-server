#define SBS_LOG_COMP "api-auth"

#include "sbs/api_server.h"
#include "sbs/auth_manager.h"

static const char *json_str(cJSON *obj, const char *key)
{
    if (!cJSON_IsObject(obj)) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

static bool json_bool(cJSON *obj, const char *key)
{
    if (!cJSON_IsObject(obj)) return false;
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static cJSON *login_result(sbs_api_server_t *server, char *token)
{
    cJSON *result = sbs_auth_manager_status_json(server->auth);
    cJSON_AddStringToObject(result, "token", token ? token : "");
    g_free(token);
    return result;
}

int sbs_api_handle_auth_status(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    (void)params;
    (void)error;
    *result = sbs_auth_manager_status_json(server->auth);
    return SBS_OK;
}

int sbs_api_handle_auth_setup(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    char *token = NULL;
    int rc;

    if (json_bool(params, "passwordless")) {
        if (!sbs_auth_manager_setup_required(server->auth)) {
            *error = api_error(-32010, "Invalid setup request");
            return SBS_ERR_INVAL;
        }
        rc = sbs_auth_manager_set_passwordless(server->auth, true);
        if (rc != SBS_OK) {
            *error = api_error(-32010, "Invalid setup request");
            return rc;
        }
        if (client) client->authenticated = true;
        *result = login_result(server, NULL);
        return SBS_OK;
    }

    rc = sbs_auth_manager_setup(server->auth,
                                json_str(params, "username"),
                                json_str(params, "password"),
                                &token);
    if (rc != SBS_OK) {
        *error = api_error(-32010, "Invalid setup request");
        return rc;
    }
    if (client) client->authenticated = true;
    *result = login_result(server, token);
    return SBS_OK;
}

int sbs_api_handle_auth_login(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    char *token = NULL;
    int rc = sbs_auth_manager_login_password(server->auth,
                                             json_str(params, "username"),
                                             json_str(params, "password"),
                                             &token);
    if (rc != SBS_OK) {
        *error = api_error(-32011, "Invalid username or password");
        return rc;
    }
    if (client) client->authenticated = true;
    *result = login_result(server, token);
    return SBS_OK;
}

int sbs_api_handle_auth_login_api_key(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    char *token = NULL;
    int rc = sbs_auth_manager_login_api_key(server->auth,
                                           json_str(params, "api_key"),
                                           &token);
    if (rc != SBS_OK) {
        *error = api_error(-32012, "Invalid API key");
        return rc;
    }
    if (client) client->authenticated = true;
    *result = login_result(server, token);
    return SBS_OK;
}

int sbs_api_handle_auth_create_api_key(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error)
{
    char *api_key = NULL;
    cJSON *metadata = NULL;
    int rc;
    (void)client;
    rc = sbs_auth_manager_create_api_key(server->auth,
                                         json_str(params, "name"),
                                         &api_key,
                                         &metadata);
    if (rc != SBS_OK) {
        *error = api_error(-32013, "Failed to create API key");
        return rc;
    }
    *result = metadata ? metadata : cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "api_key", api_key);
    g_free(api_key);
    return SBS_OK;
}

int sbs_api_handle_auth_list_api_keys(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *keys = NULL;
    int rc;
    (void)client;
    (void)params;
    rc = sbs_auth_manager_list_api_keys(server->auth, &keys);
    if (rc != SBS_OK) {
        *error = api_error(-32016, "Failed to list API keys");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddItemToObject(*result, "api_keys", keys);
    return SBS_OK;
}

int sbs_api_handle_auth_delete_api_key(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_auth_manager_delete_api_key(server->auth, json_str(params, "id"));
    if (rc != SBS_OK) {
        *error = api_error(-32017, "Failed to delete API key");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "deleted", true);
    return SBS_OK;
}

int sbs_api_handle_auth_update_credentials(sbs_api_server_t *server, sbs_api_client_t *client,
                                           cJSON *params, cJSON **result, cJSON **error)
{
    char *token = NULL;
    int rc = sbs_auth_manager_update_credentials(server->auth,
                                                 json_str(params, "username"),
                                                 json_str(params, "password"),
                                                 &token);
    if (rc != SBS_OK) {
        *error = api_error(-32014, "Failed to update credentials");
        return rc;
    }
    if (client) client->authenticated = true;
    *result = login_result(server, token);
    return SBS_OK;
}

int sbs_api_handle_auth_set_passwordless(sbs_api_server_t *server, sbs_api_client_t *client,
                                         cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled");
    bool passwordless = cJSON_IsTrue(enabled);
    int rc = sbs_auth_manager_set_passwordless(server->auth, passwordless);
    (void)client;
    if (rc != SBS_OK) {
        *error = api_error(-32015, "Failed to update passwordless mode");
        return rc;
    }
    *result = sbs_auth_manager_status_json(server->auth);
    return SBS_OK;
}
