#define SBS_LOG_COMP "instance-mgr"

#include "sbs/instance_manager.h"
#include "sbs/log.h"
#include "sbs/types.h"

#include <cjson/cJSON.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct sbs_instance_entry {
    uint32_t instance_id;
    char *name;
    bool desired_running;
    bool running;
    GPid pid;
    uint16_t api_port;
    uint16_t preview_port;
    char *config_dir;
    char *control_socket_path;
    char *log_path;
} sbs_instance_entry_t;

struct sbs_instance_manager {
    char *server_bin;
    char *controller_config_dir;
    char *instances_path;
    char *instances_root;
    char *socket_root;
    char *log_level;
    GHashTable *entries;
    uint32_t next_instance_id;
};

static void entry_free(gpointer data)
{
    sbs_instance_entry_t *entry = data;
    if (!entry) return;
    g_free(entry->name);
    g_free(entry->config_dir);
    g_free(entry->control_socket_path);
    g_free(entry->log_path);
    g_free(entry);
}

static uint16_t instance_api_port(uint32_t instance_id)
{
    return (uint16_t)(10100U + instance_id * 2U);
}

static void derive_paths(sbs_instance_manager_t *mgr, sbs_instance_entry_t *entry)
{
    char *id_str;
    entry->api_port = instance_api_port(entry->instance_id);
    entry->preview_port = (uint16_t)(entry->api_port + 1U);
    g_free(entry->config_dir);
    g_free(entry->control_socket_path);
    g_free(entry->log_path);
    id_str = g_strdup_printf("%u", entry->instance_id);
    entry->config_dir = g_build_filename(mgr->instances_root, id_str, NULL);
    entry->control_socket_path = g_build_filename(mgr->socket_root,
                                                  g_strdup_printf("instance-%u.sock", entry->instance_id),
                                                  NULL);
    entry->log_path = g_build_filename(entry->config_dir, "instance.log", NULL);
    g_free(id_str);
}

static sbs_instance_entry_t *lookup_entry(sbs_instance_manager_t *mgr, uint32_t instance_id)
{
    return mgr ? g_hash_table_lookup(mgr->entries, GUINT_TO_POINTER(instance_id)) : NULL;
}

static bool name_in_use(sbs_instance_manager_t *mgr, const char *name, uint32_t ignore_instance_id)
{
    GHashTableIter iter;
    gpointer value;
    if (!mgr || !name || !*name) return false;
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_instance_entry_t *entry = value;
        if (entry->instance_id != ignore_instance_id && g_strcmp0(entry->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static char *default_instance_name(uint32_t instance_id)
{
    return instance_id == 0 ? g_strdup("Default") : g_strdup_printf("Instance %u", instance_id);
}

/* ── Child watch: detect instance crashes and auto-restart ──── */

typedef struct {
    sbs_instance_manager_t *mgr;
    uint32_t instance_id;
} child_watch_ctx_t;

/* Forward declaration */
static int spawn_instance(sbs_instance_manager_t *mgr, sbs_instance_entry_t *entry);

static gboolean restart_instance_idle(gpointer user_data)
{
    child_watch_ctx_t *ctx = user_data;
    sbs_instance_entry_t *entry = lookup_entry(ctx->mgr, ctx->instance_id);
    if (entry && entry->desired_running && !entry->running) {
        LOG_I("auto-restarting instance %u after crash", ctx->instance_id);
        spawn_instance(ctx->mgr, entry);
    }
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void on_instance_child_exit(GPid pid, gint status, gpointer user_data)
{
    child_watch_ctx_t *ctx = user_data;
    sbs_instance_entry_t *entry = lookup_entry(ctx->mgr, ctx->instance_id);

    if (WIFEXITED(status)) {
        LOG_W("instance %u (pid %d) exited with code %d",
              ctx->instance_id, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LOG_W("instance %u (pid %d) killed by signal %d",
              ctx->instance_id, (int)pid, WTERMSIG(status));
    } else {
        LOG_W("instance %u (pid %d) exited (status=0x%x)",
              ctx->instance_id, (int)pid, status);
    }

    g_spawn_close_pid(pid);

    if (entry) {
        entry->running = false;
        entry->pid = 0;

        if (entry->desired_running) {
            /* Schedule restart on next main loop iteration to avoid
             * re-entrancy issues with spawn_instance */
            child_watch_ctx_t *restart_ctx = g_new0(child_watch_ctx_t, 1);
            restart_ctx->mgr = ctx->mgr;
            restart_ctx->instance_id = ctx->instance_id;
            g_idle_add(restart_instance_idle, restart_ctx);
        }
    }

    g_free(ctx);
}

static void refresh_entry_state(sbs_instance_entry_t *entry)
{
    int status = 0;
    pid_t rc;
    if (!entry || !entry->running || entry->pid <= 0) return;
    rc = waitpid(entry->pid, &status, WNOHANG);
    if (rc == 0) {
        return;
    }
    if (rc == entry->pid) {
        entry->running = false;
        entry->pid = 0;
    }
}

static void refresh_all_states(sbs_instance_manager_t *mgr)
{
    GHashTableIter iter;
    gpointer value;
    if (!mgr) return;
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        refresh_entry_state(value);
    }
}

static int save_metadata(sbs_instance_manager_t *mgr)
{
    GHashTableIter iter;
    gpointer value;
    cJSON *root;
    cJSON *instances;
    char *json;
    int rc = SBS_OK;
    if (!mgr) return SBS_ERR_INVAL;
    root = cJSON_CreateObject();
    instances = cJSON_CreateArray();
    cJSON_AddNumberToObject(root, "next_instance_id", (double)mgr->next_instance_id);
    cJSON_AddItemToObject(root, "instances", instances);
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_instance_entry_t *entry = value;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "instance_id", (double)entry->instance_id);
        cJSON_AddStringToObject(obj, "name", entry->name ? entry->name : "");
        cJSON_AddBoolToObject(obj, "desired_running", entry->desired_running);
        cJSON_AddItemToArray(instances, obj);
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!g_file_set_contents(mgr->instances_path, json, -1, NULL)) {
        rc = SBS_ERR_IO;
    }
    g_free(json);
    return rc;
}

static int load_metadata(sbs_instance_manager_t *mgr)
{
    char *contents = NULL;
    gsize len = 0;
    cJSON *root;
    cJSON *instances;
    int i;
    if (!mgr) return SBS_ERR_INVAL;
    if (!g_file_get_contents(mgr->instances_path, &contents, &len, NULL)) {
        return SBS_ERR_NOT_FOUND;
    }
    (void)len;
    root = cJSON_Parse(contents);
    g_free(contents);
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return SBS_ERR_INVAL;
    }
    {
        cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "next_instance_id");
        mgr->next_instance_id = cJSON_IsNumber(next) ? (uint32_t)next->valueint : 1U;
    }
    if (mgr->next_instance_id == 0) mgr->next_instance_id = 1;
    instances = cJSON_GetObjectItemCaseSensitive(root, "instances");
    if (cJSON_IsArray(instances)) {
        for (i = 0; i < cJSON_GetArraySize(instances); i++) {
            cJSON *obj = cJSON_GetArrayItem(instances, i);
            sbs_instance_entry_t *entry;
            uint32_t instance_id;
            if (!cJSON_IsObject(obj)) continue;
            {
                cJSON *id_item = cJSON_GetObjectItemCaseSensitive(obj, "instance_id");
                instance_id = cJSON_IsNumber(id_item) ? (uint32_t)id_item->valueint : 0U;
            }
            entry = g_new0(sbs_instance_entry_t, 1);
            entry->instance_id = instance_id;
            {
                cJSON *name_item = cJSON_GetObjectItemCaseSensitive(obj, "name");
                entry->name = cJSON_IsString(name_item) && cJSON_GetStringValue(name_item)
                    ? g_strdup(cJSON_GetStringValue(name_item))
                    : default_instance_name(instance_id);
            }
            entry->desired_running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "desired_running"));
            derive_paths(mgr, entry);
            g_hash_table_insert(mgr->entries, GUINT_TO_POINTER(entry->instance_id), entry);
            if (instance_id >= mgr->next_instance_id) {
                mgr->next_instance_id = instance_id + 1;
            }
        }
    }
    cJSON_Delete(root);
    return SBS_OK;
}

sbs_instance_manager_t *sbs_instance_manager_new(const char *server_bin,
                                                 const char *controller_config_dir,
                                                 const char *socket_root,
                                                 const char *log_level)
{
    sbs_instance_manager_t *mgr = g_new0(sbs_instance_manager_t, 1);
    if (!mgr) return NULL;
    mgr->server_bin = g_strdup(server_bin);
    mgr->controller_config_dir = g_strdup(controller_config_dir);
    mgr->instances_root = g_build_filename(controller_config_dir, "instances", NULL);
    mgr->instances_path = g_build_filename(controller_config_dir, "instances.json", NULL);
    mgr->socket_root = g_strdup(socket_root);
    mgr->log_level = g_strdup(log_level ? log_level : "info");
    mgr->entries = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, entry_free);
    mgr->next_instance_id = 1;
    g_mkdir_with_parents(mgr->controller_config_dir, 0755);
    g_mkdir_with_parents(mgr->instances_root, 0755);
    g_mkdir_with_parents(mgr->socket_root, 0755);
    return mgr;
}

void sbs_instance_manager_free(sbs_instance_manager_t *mgr)
{
    if (!mgr) return;
    sbs_instance_manager_shutdown_all(mgr);
    g_hash_table_destroy(mgr->entries);
    g_free(mgr->server_bin);
    g_free(mgr->controller_config_dir);
    g_free(mgr->instances_root);
    g_free(mgr->instances_path);
    g_free(mgr->socket_root);
    g_free(mgr->log_level);
    g_free(mgr);
}

int sbs_instance_manager_load(sbs_instance_manager_t *mgr)
{
    int rc = load_metadata(mgr);
    if (rc == SBS_ERR_NOT_FOUND) return SBS_OK;
    return rc;
}

int sbs_instance_manager_save(sbs_instance_manager_t *mgr)
{
    return save_metadata(mgr);
}

int sbs_instance_manager_ensure_default(sbs_instance_manager_t *mgr)
{
    sbs_instance_entry_t *entry;
    if (!mgr) return SBS_ERR_INVAL;
    entry = lookup_entry(mgr, 0);
    if (entry) return SBS_OK;
    entry = g_new0(sbs_instance_entry_t, 1);
    entry->instance_id = 0;
    entry->name = default_instance_name(0);
    entry->desired_running = true;
    derive_paths(mgr, entry);
    g_hash_table_insert(mgr->entries, GUINT_TO_POINTER(0), entry);
    if (mgr->next_instance_id == 0) mgr->next_instance_id = 1;
    return save_metadata(mgr);
}

size_t sbs_instance_manager_count(sbs_instance_manager_t *mgr)
{
    return mgr ? g_hash_table_size(mgr->entries) : 0;
}

int sbs_instance_manager_get_info(sbs_instance_manager_t *mgr,
                                  uint32_t instance_id,
                                  sbs_instance_info_t *info)
{
    sbs_instance_entry_t *entry;
    if (!mgr || !info) return SBS_ERR_INVAL;
    refresh_all_states(mgr);
    entry = lookup_entry(mgr, instance_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    memset(info, 0, sizeof(*info));
    info->instance_id = entry->instance_id;
    info->name = entry->name;
    info->desired_running = entry->desired_running;
    info->running = entry->running;
    info->pid = entry->pid;
    info->api_port = entry->api_port;
    info->preview_port = entry->preview_port;
    info->config_dir = entry->config_dir;
    info->control_socket_path = entry->control_socket_path;
    info->log_path = entry->log_path;
    return SBS_OK;
}

static cJSON *serialize_entry(sbs_instance_entry_t *entry)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "instance_id", (double)entry->instance_id);
    cJSON_AddStringToObject(obj, "name", entry->name ? entry->name : "");
    cJSON_AddBoolToObject(obj, "desired_running", entry->desired_running);
    cJSON_AddBoolToObject(obj, "running", entry->running);
    cJSON_AddNumberToObject(obj, "pid", (double)entry->pid);
    cJSON_AddNumberToObject(obj, "api_port", (double)entry->api_port);
    cJSON_AddNumberToObject(obj, "preview_port", (double)entry->preview_port);
    cJSON_AddStringToObject(obj, "config_dir", entry->config_dir ? entry->config_dir : "");
    cJSON_AddStringToObject(obj, "control_socket_path", entry->control_socket_path ? entry->control_socket_path : "");
    cJSON_AddStringToObject(obj, "log_path", entry->log_path ? entry->log_path : "");
    return obj;
}

cJSON *sbs_instance_manager_build_inventory(sbs_instance_manager_t *mgr)
{
    GHashTableIter iter;
    gpointer value;
    cJSON *result;
    cJSON *instances;
    if (!mgr) return NULL;
    refresh_all_states(mgr);
    result = cJSON_CreateObject();
    instances = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "instances", instances);
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        cJSON_AddItemToArray(instances, serialize_entry(value));
    }
    return result;
}

static void child_setup(gpointer user_data)
{
    const char *log_path = user_data;
    int fd;

    /* Create a new session so the child is fully detached from the
       controller's terminal/session. This prevents SSH hangs when
       the controller is started from an interactive shell. */
    setsid();

    /* Redirect stdout/stderr to the instance log file so the child
       does not inherit the controller's fds (which may be owned by
       systemd or an SSH session). */
    fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    /* Close stdin — instances don't need it. */
    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        if (fd > STDIN_FILENO) close(fd);
    }
}

static int spawn_instance(sbs_instance_manager_t *mgr, sbs_instance_entry_t *entry)
{
    gchar *argv[16];
    gchar *instance_id_str;
    gchar *api_port_str;
    GError *error = NULL;
    GPid pid = 0;
    gint argc = 0;
    if (!mgr || !entry) return SBS_ERR_INVAL;
    if (entry->running) {
        LOG_D("instance %u already running (pid %d), skipping spawn",
              entry->instance_id, (int)entry->pid);
        return SBS_OK;
    }
    g_mkdir_with_parents(entry->config_dir, 0755);
    g_remove(entry->control_socket_path);
    instance_id_str = g_strdup_printf("%u", entry->instance_id);
    api_port_str = g_strdup_printf("%u", (unsigned)entry->api_port);
    argv[argc++] = mgr->server_bin;
    argv[argc++] = "--config-dir";
    argv[argc++] = entry->config_dir;
    argv[argc++] = "--instance-id";
    argv[argc++] = instance_id_str;
    argv[argc++] = "--instance-socket";
    argv[argc++] = entry->control_socket_path;
    argv[argc++] = "--api-port";
    argv[argc++] = api_port_str;
    argv[argc++] = "--log-level";
    argv[argc++] = mgr->log_level;
    argv[argc] = NULL;
    LOG_I("spawning instance %u: bin=%s config=%s socket=%s port=%s log=%s",
          entry->instance_id, mgr->server_bin, entry->config_dir,
          entry->control_socket_path, api_port_str, entry->log_path);
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                       child_setup, entry->log_path, &pid, &error)) {
        LOG_E("failed to spawn instance %u: %s",
              entry->instance_id, error ? error->message : "unknown error");
        if (error) g_error_free(error);
        g_free(instance_id_str);
        g_free(api_port_str);
        return SBS_ERR_IO;
    }
    LOG_I("instance %u spawned (pid %d)", entry->instance_id, (int)pid);
    entry->pid = pid;
    entry->running = true;
    entry->desired_running = true;

    /* Watch for child exit to detect crashes and auto-restart */
    child_watch_ctx_t *watch_ctx = g_new0(child_watch_ctx_t, 1);
    watch_ctx->mgr = mgr;
    watch_ctx->instance_id = entry->instance_id;
    g_child_watch_add(pid, on_instance_child_exit, watch_ctx);

    g_free(instance_id_str);
    g_free(api_port_str);
    return save_metadata(mgr);
}

int sbs_instance_manager_create_instance(sbs_instance_manager_t *mgr,
                                         const char *name,
                                         uint32_t *out_instance_id)
{
    sbs_instance_entry_t *entry;
    if (!mgr) return SBS_ERR_INVAL;
    if (name && *name && name_in_use(mgr, name, UINT32_MAX)) return SBS_ERR_INVAL;
    entry = g_new0(sbs_instance_entry_t, 1);
    entry->instance_id = mgr->next_instance_id++;
    entry->name = (name && *name) ? g_strdup(name) : default_instance_name(entry->instance_id);
    entry->desired_running = true;
    derive_paths(mgr, entry);
    g_hash_table_insert(mgr->entries, GUINT_TO_POINTER(entry->instance_id), entry);
    save_metadata(mgr);
    spawn_instance(mgr, entry);
    if (out_instance_id) *out_instance_id = entry->instance_id;
    return SBS_OK;
}

int sbs_instance_manager_update_instance(sbs_instance_manager_t *mgr,
                                         uint32_t instance_id,
                                         const char *name,
                                         bool *desired_running)
{
    sbs_instance_entry_t *entry;
    int rc;
    if (!mgr) return SBS_ERR_INVAL;
    refresh_all_states(mgr);
    entry = lookup_entry(mgr, instance_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    if (name && *name) {
        if (name_in_use(mgr, name, instance_id)) return SBS_ERR_INVAL;
        g_free(entry->name);
        entry->name = g_strdup(name);
    }
    if (desired_running) {
        if (*desired_running) {
            rc = spawn_instance(mgr, entry);
            if (rc != SBS_OK) return rc;
        } else {
            rc = sbs_instance_manager_stop_instance(mgr, instance_id);
            if (rc != SBS_OK) return rc;
        }
    }
    return save_metadata(mgr);
}

int sbs_instance_manager_start_instance(sbs_instance_manager_t *mgr,
                                        uint32_t instance_id)
{
    sbs_instance_entry_t *entry;
    if (!mgr) return SBS_ERR_INVAL;
    refresh_all_states(mgr);
    entry = lookup_entry(mgr, instance_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    return spawn_instance(mgr, entry);
}

int sbs_instance_manager_stop_instance(sbs_instance_manager_t *mgr,
                                       uint32_t instance_id)
{
    sbs_instance_entry_t *entry;
    int status = 0;
    if (!mgr) return SBS_ERR_INVAL;
    refresh_all_states(mgr);
    entry = lookup_entry(mgr, instance_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    entry->desired_running = false;
    if (entry->running && entry->pid > 0) {
        kill(entry->pid, SIGTERM);
        waitpid(entry->pid, &status, 0);
        entry->running = false;
        entry->pid = 0;
    }
    g_remove(entry->control_socket_path);
    return save_metadata(mgr);
}

int sbs_instance_manager_remove_instance(sbs_instance_manager_t *mgr,
                                         uint32_t instance_id)
{
    if (!mgr) return SBS_ERR_INVAL;
    if (instance_id == 0) return SBS_ERR_INVAL;
    if (sbs_instance_manager_stop_instance(mgr, instance_id) != SBS_OK) return SBS_ERR_NOT_FOUND;
    g_hash_table_remove(mgr->entries, GUINT_TO_POINTER(instance_id));
    return save_metadata(mgr);
}

int sbs_instance_manager_start_desired(sbs_instance_manager_t *mgr)
{
    GHashTableIter iter;
    gpointer value;
    int count = 0, started = 0;
    if (!mgr) return SBS_ERR_INVAL;
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_instance_entry_t *entry = value;
        count++;
        if (entry->desired_running) {
            LOG_I("starting desired instance %u (%s)",
                  entry->instance_id, entry->name ? entry->name : "");
            int rc = spawn_instance(mgr, entry);
            if (rc == SBS_OK) {
                started++;
            } else {
                LOG_E("failed to start instance %u: rc=%d",
                      entry->instance_id, rc);
            }
        } else {
            LOG_D("instance %u not desired_running, skipping",
                  entry->instance_id);
        }
    }
    LOG_I("start_desired: %d/%d instances started", started, count);
    return SBS_OK;
}

void sbs_instance_manager_shutdown_all(sbs_instance_manager_t *mgr)
{
    GHashTableIter iter;
    gpointer value;
    if (!mgr) return;
    g_hash_table_iter_init(&iter, mgr->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sbs_instance_entry_t *entry = value;
        if (entry->running && entry->pid > 0) {
            kill(entry->pid, SIGTERM);
            waitpid(entry->pid, NULL, 0);
            entry->running = false;
            entry->pid = 0;
        }
        g_remove(entry->control_socket_path);
    }
}

static int read_line_response(GSocketConnection *conn, char **out_line)
{
    GInputStream *in;
    GDataInputStream *din;
    gsize len = 0;
    char *line;
    if (!conn || !out_line) return SBS_ERR_INVAL;
    in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    din = g_data_input_stream_new(in);
    line = g_data_input_stream_read_line(din, &len, NULL, NULL);
    g_object_unref(din);
    if (!line) return SBS_ERR_IO;
    *out_line = line;
    return SBS_OK;
}

int sbs_instance_manager_call_json(sbs_instance_manager_t *mgr,
                                   uint32_t instance_id,
                                   const char *request_json,
                                   char **response_json)
{
    sbs_instance_entry_t *entry;
    GSocketClient *client;
    GSocketConnection *conn;
    GSocketAddress *addr;
    GOutputStream *out;
    char *request_line;
    int rc;
    int attempt;
    if (!mgr || !request_json || !response_json) return SBS_ERR_INVAL;
    refresh_all_states(mgr);
    entry = lookup_entry(mgr, instance_id);
    if (!entry) return SBS_ERR_NOT_FOUND;
    if (!entry->running) return SBS_ERR_IO;
    client = g_socket_client_new();
    addr = g_unix_socket_address_new(entry->control_socket_path);
    conn = NULL;
    for (attempt = 0; attempt < 50 && !conn; attempt++) {
        conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
        if (!conn) g_usleep(100000);
    }
    g_object_unref(addr);
    if (!conn) {
        g_object_unref(client);
        return SBS_ERR_IO;
    }
    out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    request_line = g_strconcat(request_json, "\n", NULL);
    if (!g_output_stream_write_all(out, request_line, strlen(request_line), NULL, NULL, NULL)) {
        g_free(request_line);
        g_object_unref(conn);
        g_object_unref(client);
        return SBS_ERR_IO;
    }
    g_output_stream_flush(out, NULL, NULL);
    g_free(request_line);
    rc = read_line_response(conn, response_json);
    g_object_unref(conn);
    g_object_unref(client);
    return rc;
}
