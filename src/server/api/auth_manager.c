#define SBS_LOG_COMP "auth"

#include "sbs/auth_manager.h"
#include "sbs/log.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct sbs_api_key_record {
    char *id;
    char *name;
    char *salt;
    char *hash;
    char *created_at;
} sbs_api_key_record_t;

struct sbs_auth_manager {
    char *path;
    char *mode;
    char *username;
    char *password_salt;
    char *password_hash;
    GPtrArray *api_keys;
    GHashTable *sessions;
};

static void api_key_record_free(sbs_api_key_record_t *key)
{
    if (!key) return;
    g_free(key->id);
    g_free(key->name);
    g_free(key->salt);
    g_free(key->hash);
    g_free(key->created_at);
    g_free(key);
}

static char *random_token(size_t bytes)
{
    guint8 *buf = g_malloc0(bytes);
    char *encoded;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < bytes) {
            ssize_t n = read(fd, buf + off, bytes - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(fd);
    }
    for (size_t i = 0; i < bytes; i++) {
        if (buf[i] == 0)
            buf[i] = (guint8)g_random_int_range(1, 255);
    }
    encoded = g_base64_encode(buf, bytes);
    g_free(buf);
    for (char *p = encoded; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') *p = '0';
    }
    return encoded;
}

static char *hash_secret(const char *salt, const char *secret)
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    char *out;
    g_checksum_update(sum, (const guchar *)salt, strlen(salt));
    g_checksum_update(sum, (const guchar *)":", 1);
    g_checksum_update(sum, (const guchar *)secret, strlen(secret));
    out = g_strdup(g_checksum_get_string(sum));
    g_checksum_free(sum);
    return out;
}

static bool secure_equals(const char *a, const char *b)
{
    size_t alen, blen, max;
    unsigned char diff = 0;
    if (!a || !b) return false;
    alen = strlen(a);
    blen = strlen(b);
    max = alen > blen ? alen : blen;
    for (size_t i = 0; i < max; i++) {
        unsigned char ac = i < alen ? (unsigned char)a[i] : 0;
        unsigned char bc = i < blen ? (unsigned char)b[i] : 0;
        diff |= ac ^ bc;
    }
    return diff == 0 && alen == blen;
}

static char *now_iso8601(void)
{
    GDateTime *dt = g_date_time_new_now_utc();
    char *out = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(dt);
    return out;
}

static void clear_config(sbs_auth_manager_t *auth)
{
    if (!auth) return;
    g_clear_pointer(&auth->mode, g_free);
    g_clear_pointer(&auth->username, g_free);
    g_clear_pointer(&auth->password_salt, g_free);
    g_clear_pointer(&auth->password_hash, g_free);
    if (auth->api_keys)
        g_ptr_array_set_size(auth->api_keys, 0);
}

static void clear_login_config(sbs_auth_manager_t *auth)
{
    if (!auth) return;
    g_clear_pointer(&auth->mode, g_free);
    g_clear_pointer(&auth->username, g_free);
    g_clear_pointer(&auth->password_salt, g_free);
    g_clear_pointer(&auth->password_hash, g_free);
}

static int save_config(sbs_auth_manager_t *auth)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *keys = cJSON_CreateArray();
    char *json;
    int rc = SBS_OK;

    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "mode", auth->mode ? auth->mode : "password");
    if (auth->username) cJSON_AddStringToObject(root, "username", auth->username);
    if (auth->password_salt) cJSON_AddStringToObject(root, "password_salt", auth->password_salt);
    if (auth->password_hash) cJSON_AddStringToObject(root, "password_hash", auth->password_hash);
    for (guint i = 0; i < auth->api_keys->len; i++) {
        sbs_api_key_record_t *key = g_ptr_array_index(auth->api_keys, i);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", key->id);
        cJSON_AddStringToObject(obj, "name", key->name ? key->name : "API Key");
        cJSON_AddStringToObject(obj, "salt", key->salt);
        cJSON_AddStringToObject(obj, "hash", key->hash);
        cJSON_AddStringToObject(obj, "created_at", key->created_at ? key->created_at : "");
        cJSON_AddItemToArray(keys, obj);
    }
    cJSON_AddItemToObject(root, "api_keys", keys);
    json = cJSON_PrintUnformatted(root);
    if (!json || g_file_set_contents(auth->path, json, -1, NULL) == FALSE) {
        rc = SBS_ERR_IO;
    } else {
        chmod(auth->path, S_IRUSR | S_IWUSR);
    }
    g_free(json);
    cJSON_Delete(root);
    return rc;
}

static void load_config(sbs_auth_manager_t *auth)
{
    gchar *contents = NULL;
    gsize len = 0;
    cJSON *root;

    clear_config(auth);
    if (!g_file_get_contents(auth->path, &contents, &len, NULL))
        return;

    root = cJSON_Parse(contents);
    g_free(contents);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON *salt = cJSON_GetObjectItemCaseSensitive(root, "password_salt");
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "password_hash");
    auth->mode = g_strdup(cJSON_IsString(mode) ? cJSON_GetStringValue(mode) : "password");
    auth->username = cJSON_IsString(username) ? g_strdup(cJSON_GetStringValue(username)) : NULL;
    auth->password_salt = cJSON_IsString(salt) ? g_strdup(cJSON_GetStringValue(salt)) : NULL;
    auth->password_hash = cJSON_IsString(hash) ? g_strdup(cJSON_GetStringValue(hash)) : NULL;

    cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "api_keys");
    if (cJSON_IsArray(keys)) {
        for (int i = 0; i < cJSON_GetArraySize(keys); i++) {
            cJSON *item = cJSON_GetArrayItem(keys, i);
            cJSON *kid = cJSON_GetObjectItemCaseSensitive(item, "id");
            cJSON *kname = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *ksalt = cJSON_GetObjectItemCaseSensitive(item, "salt");
            cJSON *khash = cJSON_GetObjectItemCaseSensitive(item, "hash");
            cJSON *created = cJSON_GetObjectItemCaseSensitive(item, "created_at");
            if (!cJSON_IsString(kid) || !cJSON_IsString(ksalt) || !cJSON_IsString(khash))
                continue;
            sbs_api_key_record_t *rec = g_new0(sbs_api_key_record_t, 1);
            rec->id = g_strdup(cJSON_GetStringValue(kid));
            rec->name = g_strdup(cJSON_IsString(kname) ? cJSON_GetStringValue(kname) : "API Key");
            rec->salt = g_strdup(cJSON_GetStringValue(ksalt));
            rec->hash = g_strdup(cJSON_GetStringValue(khash));
            rec->created_at = g_strdup(cJSON_IsString(created) ? cJSON_GetStringValue(created) : "");
            g_ptr_array_add(auth->api_keys, rec);
        }
    }
    cJSON_Delete(root);
}

sbs_auth_manager_t *sbs_auth_manager_new(const char *config_dir)
{
    sbs_auth_manager_t *auth = g_new0(sbs_auth_manager_t, 1);
    auth->path = g_build_filename(config_dir ? config_dir : ".", "auth.json", NULL);
    auth->api_keys = g_ptr_array_new_with_free_func((GDestroyNotify)api_key_record_free);
    auth->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    load_config(auth);
    return auth;
}

void sbs_auth_manager_free(sbs_auth_manager_t *auth)
{
    if (!auth) return;
    clear_config(auth);
    g_free(auth->path);
    g_ptr_array_free(auth->api_keys, TRUE);
    g_hash_table_destroy(auth->sessions);
    g_free(auth);
}

bool sbs_auth_manager_passwordless(const sbs_auth_manager_t *auth)
{
    return auth && g_strcmp0(auth->mode, "passwordless") == 0;
}

bool sbs_auth_manager_setup_required(const sbs_auth_manager_t *auth)
{
    if (!auth) return true;
    if (sbs_auth_manager_passwordless(auth)) return false;
    return !auth->username || !auth->password_hash || !g_file_test(auth->path, G_FILE_TEST_EXISTS);
}

cJSON *sbs_auth_manager_status_json(const sbs_auth_manager_t *auth)
{
    cJSON *obj = cJSON_CreateObject();
    bool passwordless = sbs_auth_manager_passwordless(auth);
    cJSON_AddBoolToObject(obj, "passwordless", passwordless);
    cJSON_AddBoolToObject(obj, "setup_required", sbs_auth_manager_setup_required(auth));
    cJSON_AddBoolToObject(obj, "auth_required", !passwordless);
    if (auth && auth->username) cJSON_AddStringToObject(obj, "username", auth->username);
    return obj;
}

int sbs_auth_manager_setup(sbs_auth_manager_t *auth,
                           const char *username,
                           const char *password,
                           char **session_token_out)
{
    if (!auth || !username || !*username || !password || strlen(password) < 1)
        return SBS_ERR_INVAL;
    if (!sbs_auth_manager_setup_required(auth))
        return SBS_ERR_INVAL;
    clear_config(auth);
    auth->mode = g_strdup("password");
    auth->username = g_strdup(username);
    auth->password_salt = random_token(18);
    auth->password_hash = hash_secret(auth->password_salt, password);
    if (save_config(auth) != SBS_OK)
        return SBS_ERR_IO;
    return sbs_auth_manager_login_password(auth, username, password, session_token_out);
}

int sbs_auth_manager_login_password(sbs_auth_manager_t *auth,
                                    const char *username,
                                    const char *password,
                                    char **session_token_out)
{
    char *hash;
    char *token;
    if (!auth || sbs_auth_manager_setup_required(auth) ||
        !username || !password || g_strcmp0(username, auth->username) != 0)
        return SBS_ERR_INVAL;
    hash = hash_secret(auth->password_salt, password);
    if (!secure_equals(hash, auth->password_hash)) {
        g_free(hash);
        return SBS_ERR_INVAL;
    }
    g_free(hash);
    token = random_token(32);
    g_hash_table_insert(auth->sessions, g_strdup(token), GINT_TO_POINTER(1));
    if (session_token_out) *session_token_out = token;
    else g_free(token);
    return SBS_OK;
}

bool sbs_auth_manager_validate_session(sbs_auth_manager_t *auth,
                                       const char *session_token)
{
    if (!auth || sbs_auth_manager_passwordless(auth)) return true;
    return session_token && g_hash_table_contains(auth->sessions, session_token);
}

bool sbs_auth_manager_validate_api_key(sbs_auth_manager_t *auth,
                                       const char *api_key)
{
    if (!auth || !api_key || sbs_auth_manager_setup_required(auth)) return false;
    for (guint i = 0; i < auth->api_keys->len; i++) {
        sbs_api_key_record_t *key = g_ptr_array_index(auth->api_keys, i);
        char *hash = hash_secret(key->salt, api_key);
        bool ok = secure_equals(hash, key->hash);
        g_free(hash);
        if (ok) return true;
    }
    return false;
}

int sbs_auth_manager_login_api_key(sbs_auth_manager_t *auth,
                                   const char *api_key,
                                   char **session_token_out)
{
    char *token;
    if (!sbs_auth_manager_validate_api_key(auth, api_key))
        return SBS_ERR_INVAL;
    token = random_token(32);
    g_hash_table_insert(auth->sessions, g_strdup(token), GINT_TO_POINTER(1));
    if (session_token_out) *session_token_out = token;
    else g_free(token);
    return SBS_OK;
}

int sbs_auth_manager_create_api_key(sbs_auth_manager_t *auth,
                                    const char *name,
                                    char **api_key_out,
                                    cJSON **metadata_out)
{
    char *api_key;
    sbs_api_key_record_t *rec;
    if (!auth || sbs_auth_manager_setup_required(auth) || !api_key_out)
        return SBS_ERR_INVAL;
    char *secret = random_token(32);
    api_key = g_strconcat("sbs_", secret, NULL);
    g_free(secret);
    rec = g_new0(sbs_api_key_record_t, 1);
    rec->id = random_token(12);
    rec->name = g_strdup(name && *name ? name : "API Key");
    rec->salt = random_token(18);
    rec->hash = hash_secret(rec->salt, api_key);
    rec->created_at = now_iso8601();
    g_ptr_array_add(auth->api_keys, rec);
    if (save_config(auth) != SBS_OK)
        return SBS_ERR_IO;
    if (metadata_out) {
        *metadata_out = cJSON_CreateObject();
        cJSON_AddStringToObject(*metadata_out, "id", rec->id);
        cJSON_AddStringToObject(*metadata_out, "name", rec->name);
        cJSON_AddStringToObject(*metadata_out, "created_at", rec->created_at);
    }
    *api_key_out = api_key;
    return SBS_OK;
}

int sbs_auth_manager_list_api_keys(sbs_auth_manager_t *auth,
                                   cJSON **api_keys_out)
{
    cJSON *keys;
    if (!auth || !api_keys_out)
        return SBS_ERR_INVAL;
    keys = cJSON_CreateArray();
    for (guint i = 0; i < auth->api_keys->len; i++) {
        sbs_api_key_record_t *key = g_ptr_array_index(auth->api_keys, i);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", key->id);
        cJSON_AddStringToObject(obj, "name", key->name ? key->name : "API Key");
        cJSON_AddStringToObject(obj, "created_at", key->created_at ? key->created_at : "");
        cJSON_AddItemToArray(keys, obj);
    }
    *api_keys_out = keys;
    return SBS_OK;
}

int sbs_auth_manager_delete_api_key(sbs_auth_manager_t *auth,
                                    const char *id)
{
    if (!auth || !id || !*id)
        return SBS_ERR_INVAL;
    for (guint i = 0; i < auth->api_keys->len; i++) {
        sbs_api_key_record_t *key = g_ptr_array_index(auth->api_keys, i);
        if (g_strcmp0(key->id, id) == 0) {
            g_ptr_array_remove_index(auth->api_keys, i);
            return save_config(auth);
        }
    }
    return SBS_ERR_NOT_FOUND;
}

int sbs_auth_manager_update_credentials(sbs_auth_manager_t *auth,
                                        const char *username,
                                        const char *password,
                                        char **session_token_out)
{
    if (!auth || !username || !*username || !password || !*password)
        return SBS_ERR_INVAL;
    clear_login_config(auth);
    auth->mode = g_strdup("password");
    auth->username = g_strdup(username);
    auth->password_salt = random_token(18);
    auth->password_hash = hash_secret(auth->password_salt, password);
    g_hash_table_remove_all(auth->sessions);
    if (save_config(auth) != SBS_OK)
        return SBS_ERR_IO;
    return sbs_auth_manager_login_password(auth, username, password, session_token_out);
}

int sbs_auth_manager_set_passwordless(sbs_auth_manager_t *auth,
                                      bool passwordless)
{
    if (!auth) return SBS_ERR_INVAL;
    if (passwordless) {
        clear_login_config(auth);
        auth->mode = g_strdup("passwordless");
        g_hash_table_remove_all(auth->sessions);
        return save_config(auth);
    }
    if (!auth->username || !auth->password_hash)
        return SBS_ERR_INVAL;
    g_free(auth->mode);
    auth->mode = g_strdup("password");
    return save_config(auth);
}
