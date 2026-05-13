#define SBS_LOG_COMP "api-ws"

#include "sbs/api_server.h"
#include "sbs/log.h"
#include "sbs/telemetry_utils.h"

#include <stdio.h>

static double read_cpu_usage_pubsub(void)
{
    static uint64_t prev_idle = 0, prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;

    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0.0; }
    fclose(f);

    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    int n = sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) return 0.0;

    uint64_t idle_total = idle + (n >= 5 ? iowait : 0);
    uint64_t total = user + nice + system + idle_total
                   + (n >= 6 ? irq : 0) + (n >= 7 ? softirq : 0) + (n >= 8 ? steal : 0);

    uint64_t delta_idle = idle_total - prev_idle;
    uint64_t delta_total = total - prev_total;
    prev_idle = idle_total;
    prev_total = total;

    if (delta_total == 0) return 0.0;
    return (1.0 - (double)delta_idle / (double)delta_total) * 100.0;
}

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct {
    sbs_api_server_t *server;
    sbs_api_client_t *client;
} ws_thread_ctx_t;

static char *build_ws_frame(const char *payload)
{
    gsize len = payload ? strlen(payload) : 0;
    GByteArray *buf = g_byte_array_sized_new(len + 10);
    guint8 hdr[10];
    guint hdr_len = 0;

    hdr[hdr_len++] = 0x81;
    if (len < 126) {
        hdr[hdr_len++] = (guint8)len;
    } else {
        hdr[hdr_len++] = 126;
        hdr[hdr_len++] = (guint8)((len >> 8) & 0xff);
        hdr[hdr_len++] = (guint8)(len & 0xff);
    }
    g_byte_array_append(buf, hdr, hdr_len);
    if (len > 0) {
        g_byte_array_append(buf, (const guint8 *)payload, len);
    }
    return (char *)g_byte_array_free(buf, FALSE);
}

static char *compute_accept_key(const char *client_key)
{
    GChecksum *sum;
    guchar digest[20];
    gsize digest_len = sizeof(digest);
    char *concat = g_strconcat(client_key, WS_GUID, NULL);
    char *accept;

    sum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sum, (const guchar *)concat, strlen(concat));
    g_checksum_get_digest(sum, digest, &digest_len);
    accept = g_base64_encode(digest, digest_len);
    g_checksum_free(sum);
    g_free(concat);
    return accept;
}

static gboolean perform_handshake(GSocketConnection *connection)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GDataInputStream *din = g_data_input_stream_new(in);
    GError *error = NULL;
    char *line;
    gsize line_len = 0;
    char *key_value = NULL;
    char *accept;
    char response[512];

    while ((line = g_data_input_stream_read_line(din, &line_len, NULL, &error)) != NULL) {
        if (line_len == 0 || (line_len == 1 && line[0] == '\r')) {
            g_free(line);
            break;
        }
        if (g_str_has_prefix(line, "Sec-WebSocket-Key:")) {
            char *trim = line + strlen("Sec-WebSocket-Key:");
            while (*trim == ' ') trim++;
            g_strchomp(trim);
            key_value = g_strdup(trim);
        }
        g_free(line);
    }
    if (error) {
        g_error_free(error);
        g_object_unref(din);
        return FALSE;
    }
    if (!key_value) {
        g_object_unref(din);
        return FALSE;
    }
    accept = compute_accept_key(key_value);
    g_free(key_value);
    g_object_unref(din);
    g_snprintf(response, sizeof(response),
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n",
               accept);
    g_free(accept);
    return g_output_stream_write_all(out, response, strlen(response), NULL, NULL, NULL);
}

static gboolean recv_exact(int fd, void *buf, size_t len)
{
    guint8 *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_usleep(1000);
                continue;
            }
            return FALSE;
        }
        if (n == 0) {
            return FALSE;
        }
        off += (size_t)n;
    }
    return TRUE;
}

static char *read_ws_text_frame(int fd)
{
    guint8 hdr[2];
    guint8 mask[4];
    guint64 payload_len;
    guint8 *payload;
    guint64 i;
    guint8 opcode;

    if (!recv_exact(fd, hdr, 2)) {
        return NULL;
    }
    opcode = hdr[0] & 0x0f;
    if (opcode == 0x8) {
        return NULL;
    }
    if (opcode != 0x1) {
        return NULL;
    }
    payload_len = hdr[1] & 0x7f;
    if (payload_len == 126) {
        guint8 ext[2];
        if (!recv_exact(fd, ext, 2)) {
            return NULL;
        }
        payload_len = ((guint64)ext[0] << 8) | ext[1];
    }
    if (!(hdr[1] & 0x80)) {
        return NULL;
    }
    if (!recv_exact(fd, mask, 4)) {
        return NULL;
    }
    payload = g_malloc(payload_len + 1);
    if (!recv_exact(fd, payload, (size_t)payload_len)) {
        g_free(payload);
        return NULL;
    }
    for (i = 0; i < payload_len; i++) {
        payload[i] ^= mask[i % 4];
    }
    payload[payload_len] = '\0';
    return (char *)payload;
}

static gboolean send_ws_text(int fd, const char *payload)
{
    char *frame = build_ws_frame(payload);
    gsize len = 2 + strlen(payload ? payload : "");
    gboolean ok;
    if (strlen(payload ? payload : "") >= 126) {
        len += 2;
    }
    ok = send(fd, frame, len, 0) == (ssize_t)len;
    g_free(frame);
    return ok;
}

static gpointer client_thread_main(gpointer data)
{
    ws_thread_ctx_t *ctx = data;
    GSocketConnection *conn = G_SOCKET_CONNECTION(ctx->client->connection);
    int fd = g_socket_get_fd(g_socket_connection_get_socket(conn));

    while (ctx->server->running && ctx->client->connected) {
        char *request = read_ws_text_frame(fd);
        char *response = NULL;
        if (!request) {
            LOG_D("websocket client %lu disconnected or frame read failed",
                  (unsigned long)ctx->client->id);
            break;
        }
        LOG_D("websocket client %lu request: %s",
              (unsigned long)ctx->client->id, request);
        sbs_api_server_dispatch_json(ctx->server, ctx->client, request, &response);
        if (response) {
            LOG_D("websocket client %lu response: %s",
                  (unsigned long)ctx->client->id, response);
            send_ws_text(fd, response);
            g_free(response);
        }
        g_free(request);
    }

    ctx->client->connected = false;
    g_free(ctx);
    return NULL;
}

static gboolean on_incoming(GSocketService *service,
                            GSocketConnection *connection,
                            GObject *source_object,
                            gpointer user_data)
{
    sbs_api_server_t *server = user_data;
    sbs_api_client_t *client;
    ws_thread_ctx_t *ctx;
    (void)service;
    (void)source_object;

    /* Handshake reads run on the GLib main loop.  Do not let an idle TCP
     * preconnect or half-open client wedge every HTTP/API connection. */
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 2);
    if (!perform_handshake(connection)) {
        return FALSE;
    }

    g_socket_set_timeout(g_socket_connection_get_socket(connection), 0);
    g_socket_set_blocking(g_socket_connection_get_socket(connection), TRUE);

    client = sbs_api_client_new(0);
    client->connection = g_object_ref(connection);
    client->connected = true;
    {
        GSocketAddress *remote = g_socket_connection_get_remote_address(connection, NULL);
        if (remote && G_IS_INET_SOCKET_ADDRESS(remote)) {
            GInetAddress *addr = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));
            client->peer_ip = g_inet_address_to_string(addr);
        }
        if (remote) {
            g_object_unref(remote);
        }
    }
    sbs_api_server_add_client(server, client);

    ctx = g_new0(ws_thread_ctx_t, 1);
    ctx->server = server;
    ctx->client = client;
    client->thread = g_thread_new("sbs-api-client", client_thread_main, ctx);
    return TRUE;
}

static gboolean api_service_cb(gpointer user_data)
{
    sbs_api_server_t *server = user_data;
    GHashTableIter iter;
    gpointer key, value;

    if (!server || !server->running) {
        return G_SOURCE_REMOVE;
    }

    g_hash_table_iter_init(&iter, server->clients);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_api_client_t *client = value;
        if (client->connected && client->outbox->len > 0 && client->connection) {
            int fd = g_socket_get_fd(g_socket_connection_get_socket(G_SOCKET_CONNECTION(client->connection)));
            char *msg;
            while ((msg = sbs_api_client_take_message(client)) != NULL) {
                send_ws_text(fd, msg);
                g_free(msg);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean serve_preview_file(GSocketConnection *connection, const char *method, const char *raw_path)
{
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    const char *suffix = NULL;
    char *full_path;
    gchar *contents = NULL;
    gsize length = 0;
    const char *mime = "application/octet-stream";
    char header[256];
    char *path;
    const char *q;

    /* Strip query string from path */
    q = strchr(raw_path, '?');
    if (q) {
        path = g_strndup(raw_path, (size_t)(q - raw_path));
    } else {
        path = g_strdup(raw_path);
    }

    if (g_strcmp0(method, "GET") != 0 && g_strcmp0(method, "HEAD") != 0) {
        g_snprintf(header, sizeof(header),
                   "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        g_output_stream_write_all(out, header, strlen(header), NULL, NULL, NULL);
        g_free(path);
        return TRUE;
    }

    if (g_str_has_prefix(path, "/preview/")) {
        suffix = path + strlen("/preview/");
        if (strstr(suffix, "..")) {
            g_free(path);
            return FALSE;
        }
        full_path = g_strdup_printf("/tmp/sbs-preview/%s", suffix);
        if (g_str_has_suffix(full_path, ".m3u8")) {
            mime = "application/vnd.apple.mpegurl";
        } else if (g_str_has_suffix(full_path, ".ts")) {
            mime = "video/mp2t";
        }
    } else if (g_str_has_prefix(path, "/snapshots/")) {
        suffix = path + strlen("/snapshots/");
        if (strstr(suffix, "..")) {
            g_free(path);
            return FALSE;
        }
        full_path = g_strdup_printf("/tmp/sbs-snapshots/%s", suffix);
        mime = "image/jpeg";
    } else {
        /* Serve WebUI static files */
        if (strcmp(path, "/") == 0) {
            full_path = g_strdup("/usr/share/sbs/webui/index.html");
        } else {
            if (strstr(path, "..")) {
                g_free(path);
                return FALSE;
            }
            full_path = g_strdup_printf("/usr/share/sbs/webui%s", path);
        }
        if (g_str_has_suffix(full_path, ".html")) {
            mime = "text/html";
        } else if (g_str_has_suffix(full_path, ".js")) {
            mime = "application/javascript";
        } else if (g_str_has_suffix(full_path, ".css")) {
            mime = "text/css";
        } else if (g_str_has_suffix(full_path, ".json")) {
            mime = "application/json";
        } else if (g_str_has_suffix(full_path, ".png")) {
            mime = "image/png";
        } else if (g_str_has_suffix(full_path, ".svg")) {
            mime = "image/svg+xml";
        } else if (g_str_has_suffix(full_path, ".woff2")) {
            mime = "font/woff2";
        }
    }

    if (!g_file_get_contents(full_path, &contents, &length, NULL)) {
        g_snprintf(header, sizeof(header), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        g_output_stream_write_all(out, header, strlen(header), NULL, NULL, NULL);
        g_free(full_path);
        g_free(path);
        return TRUE;
    }

    g_snprintf(header, sizeof(header),
               "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: %s\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
               mime, (size_t)length,
               g_str_has_suffix(full_path, ".m3u8") ? "no-cache, no-store" : "max-age=86400");
    g_output_stream_write_all(out, header, strlen(header), NULL, NULL, NULL);
    if (g_strcmp0(method, "HEAD") != 0) {
        g_output_stream_write_all(out, contents, length, NULL, NULL, NULL);
    }
    g_free(contents);
    g_free(full_path);
    g_free(path);
    return TRUE;
}

static gboolean on_preview_incoming(GSocketService *service,
                                    GSocketConnection *connection,
                                    GObject *source_object,
                                    gpointer user_data)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GDataInputStream *din = g_data_input_stream_new(in);
    gchar *line;
    gsize line_len = 0;
    (void)service;
    (void)source_object;
    (void)user_data;

    /* This callback also runs on the main loop.  Browser speculative
     * connections may not send a request line; time them out quickly. */
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 2);
    line = g_data_input_stream_read_line(din, &line_len, NULL, NULL);
    if (line) {
        gchar **parts = g_strsplit(line, " ", 3);
        if (parts[0] && parts[1]) {
            serve_preview_file(connection, parts[0], parts[1]);
        }
        g_strfreev(parts);
        g_free(line);
    }
    g_object_unref(din);
    return FALSE;
}

static gboolean on_unix_incoming(GSocketService *service,
                                 GSocketConnection *connection,
                                 GObject *source_object,
                                 gpointer user_data)
{
    sbs_api_server_t *server = user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GDataInputStream *din = g_data_input_stream_new(in);
    gchar *line;
    gsize line_len = 0;
    char *response = NULL;
    (void)service;
    (void)source_object;

    line = g_data_input_stream_read_line(din, &line_len, NULL, NULL);
    if (line) {
        sbs_api_server_dispatch_json(server, NULL, line, &response);
        if (response) {
            char *wire = g_strconcat(response, "\n", NULL);
            g_output_stream_write_all(out, wire, strlen(wire), NULL, NULL, NULL);
            g_free(wire);
            g_free(response);
        }
        g_free(line);
    }
    g_object_unref(din);
    return FALSE;
}

static void on_preview_ice_candidate(void *userdata, unsigned int mline_index, const char *candidate)
{
    sbs_api_server_t *server = userdata;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "sdpMLineIndex", mline_index);
    cJSON_AddStringToObject(data, "candidate", candidate);
    sbs_api_server_publish(server, "preview.webrtc.ice", data);
}

sbs_api_server_t *sbs_api_server_new(sbs_scene_graph_t *scene_graph,
                                     sbs_source_supervisor_t *source_sup,
                                     sbs_output_supervisor_t *output_sup,
                                     sbs_compositor_thread_t *comp_thread,
                                     sbs_output_router_t *output_router)
{
    sbs_api_server_t *server = g_new0(sbs_api_server_t, 1);
    server->scene_graph = scene_graph;
    server->source_sup = source_sup;
    server->output_sup = output_sup;
    server->comp_thread = comp_thread;
    server->output_router = output_router;
    server->preview = sbs_preview_engine_new();
    if (server->preview) {
        sbs_preview_engine_set_ice_callback(server->preview, on_preview_ice_candidate, server);
    }
    server->snapshot = sbs_snapshot_engine_new();
    if (server->preview && scene_graph) {
        sbs_preview_engine_set_source_format(server->preview,
                                             scene_graph->canvas.width,
                                             scene_graph->canvas.height,
                                             scene_graph->canvas.fps_num,
                                             scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10
                                                 ? SBS_PREVIEW_COLOR_MODE_HDR10
                                                 : SBS_PREVIEW_COLOR_MODE_SDR);
    }
    if (server->output_router) {
        sbs_output_router_set_preview_engine(server->output_router, server->preview);
        sbs_output_router_set_snapshot_engine(server->output_router, server->snapshot);
    }
    /* Auto-activate the fallback preview profile so that HLS preview frames
     * start flowing immediately, without waiting for a client to call
     * preview.ensureProfile.  This also keeps the runtime alive when
     * viewer_count drops to zero (the initial ensure bumps the count to 1).
     *
     * TEMPORARILY DISABLED: fallback preview causes severe CPU contention
     * with the compositor's 4K source upload path, dropping effective fps
     * from 60 to ~30. Re-enable after GPU-based upload is implemented. */
    (void)server;
    // sbs_preview_engine_ensure_profile(server->preview, "preview-h264-720p30", NULL);
    server->methods = g_hash_table_new(g_str_hash, g_str_equal);
    server->clients = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                            (GDestroyNotify)sbs_api_client_free);
    server->next_client_id = 1;
    server->started_monotonic_usec = g_get_monotonic_time();
    sbs_api_register_core_methods(server);
    return server;
}

void sbs_api_server_free(sbs_api_server_t *server)
{
    if (!server) {
        return;
    }
    sbs_api_server_stop(server);
    sbs_preview_engine_free(server->preview);
    sbs_snapshot_engine_free(server->snapshot);
    sbs_audio_mixer_free(server->audio);
    sbs_auth_manager_free(server->auth);
    g_hash_table_destroy(server->methods);
    g_hash_table_destroy(server->clients);
    g_free(server->pending_canvas_color_mode);
    g_free(server->pending_canvas_background_color);
    g_free(server->unix_socket_path);
    g_free(server);
}

static gboolean telemetry_cb(gpointer user_data)
{
    sbs_api_server_t *server = user_data;
    sbs_api_server_publish_telemetry(server);
    return server && server->running ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean audio_level_cb(gpointer user_data)
{
    sbs_api_server_t *server = user_data;
    if (!server || !server->running) {
        return G_SOURCE_REMOVE;
    }
    sbs_api_server_publish(server, "audio.level",
                           sbs_audio_mixer_serialize_levels(server->audio));
    return G_SOURCE_CONTINUE;
}

int sbs_api_server_start(sbs_api_server_t *server, uint16_t port)
{
    if (!server) {
        return SBS_ERR_INVAL;
    }
    if (server->running) {
        return SBS_OK;
    }
    server->socket_service = g_socket_service_new();
    server->port = port;
    server->preview_port = port + 1;
    sbs_preview_engine_set_api_port(server->preview, port);
    sbs_snapshot_engine_set_api_port(server->snapshot, port);
    g_signal_connect(server->socket_service, "incoming", G_CALLBACK(on_incoming), server);
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(server->socket_service), port, NULL, NULL)) {
        g_object_unref(server->socket_service);
        server->socket_service = NULL;
        return SBS_ERR_IO;
    }
    g_socket_service_start(G_SOCKET_SERVICE(server->socket_service));
    server->preview_socket_service = g_socket_service_new();
    g_signal_connect(server->preview_socket_service, "incoming", G_CALLBACK(on_preview_incoming), server);
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(server->preview_socket_service), server->preview_port, NULL, NULL)) {
        g_object_unref(server->preview_socket_service);
        server->preview_socket_service = NULL;
        g_object_unref(server->socket_service);
        server->socket_service = NULL;
        return SBS_ERR_IO;
    }
    g_socket_service_start(G_SOCKET_SERVICE(server->preview_socket_service));
    server->running = true;
    server->service_timer_id = g_timeout_add(10, api_service_cb, server);
    server->telemetry_timer_id = g_timeout_add(1000, telemetry_cb, server);
    server->audio_level_timer_id = g_timeout_add(100, audio_level_cb, server);
    LOG_I("API server started on port %u", (unsigned)port);
    return SBS_OK;
}

int sbs_api_server_start_unix(sbs_api_server_t *server, const char *socket_path)
{
    GSocketAddress *addr;
    if (!server || !socket_path) {
        return SBS_ERR_INVAL;
    }
    if (server->unix_socket_service) {
        return SBS_OK;
    }
    g_free(server->unix_socket_path);
    server->unix_socket_path = g_strdup(socket_path);
    g_remove(socket_path);
    server->unix_socket_service = g_socket_service_new();
    g_signal_connect(server->unix_socket_service, "incoming", G_CALLBACK(on_unix_incoming), server);
    addr = g_unix_socket_address_new(socket_path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(server->unix_socket_service),
                                       addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL,
                                       NULL,
                                       NULL)) {
        g_object_unref(addr);
        g_object_unref(server->unix_socket_service);
        server->unix_socket_service = NULL;
        return SBS_ERR_IO;
    }
    g_object_unref(addr);
    g_socket_service_start(G_SOCKET_SERVICE(server->unix_socket_service));
    return SBS_OK;
}

void sbs_api_server_stop(sbs_api_server_t *server)
{
    if (!server || !server->running) {
        return;
    }
    server->running = false;
    if (server->service_timer_id) {
        g_source_remove(server->service_timer_id);
        server->service_timer_id = 0;
    }
    if (server->telemetry_timer_id) {
        g_source_remove(server->telemetry_timer_id);
        server->telemetry_timer_id = 0;
    }
    if (server->audio_level_timer_id) {
        g_source_remove(server->audio_level_timer_id);
        server->audio_level_timer_id = 0;
    }
    if (server->socket_service) {
        g_socket_service_stop(G_SOCKET_SERVICE(server->socket_service));
        g_object_unref(server->socket_service);
        server->socket_service = NULL;
    }
    if (server->preview_socket_service) {
        g_socket_service_stop(G_SOCKET_SERVICE(server->preview_socket_service));
        g_object_unref(server->preview_socket_service);
        server->preview_socket_service = NULL;
    }
    if (server->unix_socket_service) {
        g_socket_service_stop(G_SOCKET_SERVICE(server->unix_socket_service));
        g_object_unref(server->unix_socket_service);
        server->unix_socket_service = NULL;
    }
    if (server->unix_socket_path) {
        g_remove(server->unix_socket_path);
    }
}

sbs_api_client_t *sbs_api_client_new(uint64_t id)
{
    sbs_api_client_t *client = g_new0(sbs_api_client_t, 1);
    client->id = id;
    client->subscriptions = g_ptr_array_new_with_free_func(g_free);
    client->outbox = g_ptr_array_new_with_free_func(g_free);
    return client;
}

void sbs_api_client_free(sbs_api_client_t *client)
{
    if (!client) {
        return;
    }
    g_ptr_array_free(client->subscriptions, TRUE);
    g_ptr_array_free(client->outbox, TRUE);
    if (client->thread) {
        g_thread_join(client->thread);
    }
    if (client->connection) {
        g_object_unref(client->connection);
    }
    g_free(client->peer_ip);
    g_free(client);
}

uint64_t sbs_api_server_add_client(sbs_api_server_t *server, sbs_api_client_t *client)
{
    uint64_t *key;

    if (!server || !client) {
        return 0;
    }
    if (client->id == 0) {
        client->id = server->next_client_id++;
    }
    key = g_new(uint64_t, 1);
    *key = client->id;
    g_hash_table_insert(server->clients, key, client);
    return client->id;
}

void sbs_api_server_remove_client(sbs_api_server_t *server, uint64_t client_id)
{
    if (!server || client_id == 0) {
        return;
    }
    g_hash_table_remove(server->clients, &client_id);
}

void sbs_api_server_refresh_scene(sbs_api_server_t *server)
{
    sbs_comp_scene_state_t scene_state;

    if (!server || !server->scene_graph || !server->comp_thread) {
        return;
    }
    sbs_scene_graph_update_transition_runtime(server->scene_graph, g_get_monotonic_time());
    sbs_audio_mixer_on_scene_change(server->audio, server->scene_graph->active_scene_id);
    if (sbs_scene_graph_build_compositor_state(server->scene_graph, &scene_state) == SBS_OK) {
        if (scene_state.active_item_count > 0) {
            const sbs_comp_scene_item_t *item = &scene_state.active_items[0];
            LOG_I("refresh-scene pushing item=%s flags=0x%x tint=(%.3f,%.3f,%.3f)",
                  item->source_id,
                  item->filter_flags,
                  item->tint[0], item->tint[1], item->tint[2]);
        }
        sbs_compositor_thread_set_scene(server->comp_thread, &scene_state);
    }
}

void sbs_api_server_publish_telemetry(sbs_api_server_t *server)
{
    cJSON *system = cJSON_CreateObject();
    cJSON *preview = sbs_preview_serialize_telemetry(server ? server->preview : NULL);
    cJSON *audio = sbs_audio_mixer_serialize_levels(server ? server->audio : NULL);
    uint32_t frames_dropped = 0;
    uint64_t frame_count = 0;
    uint64_t content_frame_count = 0;
    uint64_t encoded_bytes = 0;
    double compositor_fps = 0.0;
    double content_fps = 0.0;
    double target_fps = 0.0;
    double bitrate_kbps = 0.0;
    double latency_ms = 0.0;
    int64_t now_usec = g_get_monotonic_time();

    if (!server) {
        cJSON_Delete(system);
        cJSON_Delete(preview);
        return;
    }

    if (server->comp_thread) {
        sbs_comp_timing_stats_t timing;
        sbs_compositor_thread_get_timing(server->comp_thread, &timing);
        frame_count = timing.frame_count;
        content_frame_count = timing.content_frame_count;
        frames_dropped = timing.frames_dropped;
        latency_ms = timing.last_frame_latency_ms > 0.0
            ? timing.last_frame_latency_ms
            : timing.last_frame_time_ms;
    }
    if (server->encoder_mgr) {
        sbs_encoder_manager_metrics_t enc_metrics;
        sbs_encoder_manager_get_metrics(server->encoder_mgr, &enc_metrics);
        encoded_bytes = enc_metrics.encoded_bytes;
    }
    if (server->output_router) {
        double encoder_time_ms = sbs_output_router_last_encoder_time_ms(server->output_router);
        if (encoder_time_ms > 0.0)
            latency_ms = encoder_time_ms;
    }
    if (server->scene_graph && server->scene_graph->canvas.fps_den > 0) {
        target_fps = (double)server->scene_graph->canvas.fps_num /
                     (double)server->scene_graph->canvas.fps_den;
    }

    if (server->last_telemetry_monotonic_usec > 0 &&
        now_usec > server->last_telemetry_monotonic_usec) {
        double dt = (double)(now_usec - server->last_telemetry_monotonic_usec) / 1000000.0;
        if (dt > 0.0) {
            uint64_t frame_delta = frame_count >= server->last_telemetry_frame_count
                ? frame_count - server->last_telemetry_frame_count
                : frame_count;
            uint64_t content_delta = content_frame_count >= server->last_telemetry_content_frame_count
                ? content_frame_count - server->last_telemetry_content_frame_count
                : content_frame_count;
            compositor_fps = (double)frame_delta / dt;
            content_fps = (double)content_delta / dt;
            if (encoded_bytes >= server->last_telemetry_encoded_bytes) {
                uint64_t byte_delta = encoded_bytes - server->last_telemetry_encoded_bytes;
                bitrate_kbps = ((double)byte_delta * 8.0) / 1000.0 / dt;
            }
        }
    } else if (server->started_monotonic_usec > 0 && now_usec > server->started_monotonic_usec) {
        double uptime_sec = (double)(now_usec - server->started_monotonic_usec) / 1000000.0;
        if (uptime_sec > 0.0) {
            compositor_fps = (double)frame_count / uptime_sec;
            content_fps = (double)content_frame_count / uptime_sec;
            bitrate_kbps = ((double)encoded_bytes * 8.0) / 1000.0 / uptime_sec;
        }
    }
    server->last_telemetry_monotonic_usec = now_usec;
    server->last_telemetry_frame_count = frame_count;
    server->last_telemetry_content_frame_count = content_frame_count;
    server->last_telemetry_encoded_bytes = encoded_bytes;
    server->last_telemetry_compositor_fps = compositor_fps;
    server->last_telemetry_content_fps = content_fps;
    server->last_telemetry_bitrate_kbps = bitrate_kbps;
    server->last_telemetry_latency_ms = latency_ms;

    bool dropped_slow = frames_dropped > 10;
    bool fps_slow =
        (target_fps > 0.0 && compositor_fps > 0.0 && compositor_fps + 0.5 < target_fps) ||
        (target_fps > 0.0 && content_frame_count > 1 && content_fps + 0.5 < target_fps);
    if (dropped_slow || fps_slow) {
        if (server->telemetry_slow_streak < UINT32_MAX) {
            server->telemetry_slow_streak++;
        }
    } else {
        server->telemetry_slow_streak = 0;
    }
    bool pipeline_slow = dropped_slow || server->telemetry_slow_streak >= 2;
    server->last_telemetry_pipeline_slow = pipeline_slow;

    sbs_preview_engine_update_metrics(server->preview,
                                       frame_count,
                                       frames_dropped,
                                       latency_ms,
                                       pipeline_slow);

    cJSON_AddNumberToObject(system, "compositor_fps", compositor_fps);
    cJSON_AddNumberToObject(system, "content_fps", content_fps);
    cJSON_AddNumberToObject(system, "frames_rendered", (double)frame_count);
    cJSON_AddNumberToObject(system, "content_frames", (double)content_frame_count);
    cJSON_AddNumberToObject(system, "frames_dropped", frames_dropped);
    cJSON_AddNumberToObject(system, "bitrate_kbps", bitrate_kbps);
    cJSON_AddNumberToObject(system, "latency_ms", latency_ms);
    cJSON_AddNumberToObject(system, "cpu_usage", read_cpu_usage_pubsub());
    cJSON_AddNumberToObject(system, "gpu_usage", sbs_telemetry_read_gpu_usage());
    cJSON_AddBoolToObject(system, "pipeline_slow", pipeline_slow);
    cJSON_AddItemToObject(system, "preview", preview);
    cJSON_AddItemToObject(system, "audio", audio);

    sbs_api_server_publish(server, "telemetry.runtime", system);
}
