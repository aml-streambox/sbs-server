#define SBS_LOG_COMP "api-system"

#include "sbs/api_server.h"
#include "sbs_version.h"

#include <stdio.h>
#include <unistd.h>

static double read_cpu_usage(void)
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

#ifndef SBS_PLATFORM
#define SBS_PLATFORM "generic"
#endif

static cJSON *err_not_found(const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", -32001);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static cJSON *serialize_runtime_health(sbs_api_server_t *server)
{
    sbs_source_supervisor_metrics_t source_metrics;
    sbs_output_supervisor_metrics_t output_metrics;
    cJSON *runtime = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *outputs = cJSON_CreateObject();
    const int64_t now = g_get_monotonic_time();
    const double uptime_sec = server->started_monotonic_usec > 0
        ? (double)(now - server->started_monotonic_usec) / 1000000.0
        : 0.0;

    memset(&source_metrics, 0, sizeof(source_metrics));
    memset(&output_metrics, 0, sizeof(output_metrics));

    sbs_source_supervisor_get_metrics(server->source_sup, &source_metrics);
    sbs_output_supervisor_get_metrics(server->output_sup, &output_metrics);

    cJSON_AddBoolToObject(runtime, "api_running", server->running);
    cJSON_AddNumberToObject(runtime, "instance_id", (double)server->instance_id);
    cJSON_AddNumberToObject(runtime, "api_port", server->port);
    cJSON_AddNumberToObject(runtime, "preview_port", server->preview_port);
    cJSON_AddNumberToObject(runtime, "uptime_sec", uptime_sec);
    cJSON_AddBoolToObject(runtime, "degraded",
                          source_metrics.degraded || output_metrics.degraded);

    cJSON_AddNumberToObject(sources, "total", source_metrics.total_sources);
    cJSON_AddNumberToObject(sources, "connected", source_metrics.connected_sources);
    cJSON_AddNumberToObject(sources, "restarting", source_metrics.restarting_sources);
    cJSON_AddNumberToObject(sources, "stale", source_metrics.stale_sources);
    cJSON_AddNumberToObject(sources, "max_restart_attempts", source_metrics.max_restart_attempts);
    cJSON_AddBoolToObject(sources, "degraded", source_metrics.degraded);

    cJSON_AddNumberToObject(outputs, "total", output_metrics.total_outputs);
    cJSON_AddNumberToObject(outputs, "connected", output_metrics.connected_outputs);
    cJSON_AddNumberToObject(outputs, "restarting", output_metrics.restarting_outputs);
    cJSON_AddNumberToObject(outputs, "max_restart_attempts", output_metrics.max_restart_attempts);
    cJSON_AddNumberToObject(outputs, "frames_sent", (double)output_metrics.frames_sent);
    cJSON_AddNumberToObject(outputs, "frames_dropped", (double)output_metrics.frames_dropped);
    cJSON_AddBoolToObject(outputs, "degraded", output_metrics.degraded);

    cJSON_AddItemToObject(runtime, "sources", sources);
    cJSON_AddItemToObject(runtime, "outputs", outputs);
    return runtime;
}

static bool pending_canvas_differs(const sbs_api_server_t *server)
{
    const char *running_mode;

    if (!server || !server->scene_graph || !server->pending_canvas_valid) {
        return false;
    }

    if (server->pending_canvas_width != server->scene_graph->canvas.width) return true;
    if (server->pending_canvas_height != server->scene_graph->canvas.height) return true;
    if (server->pending_canvas_fps_num != server->scene_graph->canvas.fps_num) return true;
    if (server->pending_canvas_fps_den != server->scene_graph->canvas.fps_den) return true;

    running_mode = server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10
        ? "hdr10" : "sdr";
    if (g_strcmp0(server->pending_canvas_color_mode, running_mode) != 0) return true;
    if (g_strcmp0(server->pending_canvas_background_color,
                  server->scene_graph->canvas.background_color) != 0) return true;

    return false;
}

static cJSON *duplicate_pending_canvas_object(const sbs_api_server_t *server)
{
    cJSON *canvas;

    if (!server || !server->pending_canvas_valid) {
        return NULL;
    }

    canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(canvas, "width", server->pending_canvas_width);
    cJSON_AddNumberToObject(canvas, "height", server->pending_canvas_height);
    cJSON_AddNumberToObject(canvas, "fps_num", server->pending_canvas_fps_num);
    cJSON_AddNumberToObject(canvas, "fps_den", server->pending_canvas_fps_den);
    cJSON_AddStringToObject(canvas, "color_mode",
                            server->pending_canvas_color_mode ?: "sdr");
    cJSON_AddStringToObject(canvas, "background_color",
                            server->pending_canvas_background_color ?: "#000000");
    return canvas;
}

static void add_native_mailbox_stats(cJSON *parent, const char *name,
                                     const sbs_native_canvas_mailbox_stats_t *stats)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "has_entry", stats->has_entry);
    cJSON_AddNumberToObject(obj, "entry_idx", stats->entry_idx);
    cJSON_AddNumberToObject(obj, "frame_number", (double)stats->frame_number);
    cJSON_AddNumberToObject(obj, "published", (double)stats->published);
    cJSON_AddNumberToObject(obj, "dropped", (double)stats->dropped);
    cJSON_AddNumberToObject(obj, "consumed", (double)stats->consumed);
    cJSON_AddNumberToObject(obj, "late", (double)stats->late);
    cJSON_AddItemToObject(parent, name, obj);
}

int sbs_api_handle_system_get_state(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    (void)params;
    if (!server || !server->scene_graph) {
        *error = err_not_found("Scene graph unavailable");
        return SBS_ERR_NOT_FOUND;
    }
    sbs_scene_graph_update_transition_runtime(server->scene_graph, g_get_monotonic_time());
    *result = sbs_scene_graph_serialize_full_state(server->scene_graph);
    if (server->preview) {
        cJSON_AddItemToObject(*result, "preview", sbs_preview_serialize_profile_catalog(server->preview));
        cJSON_AddItemToObject(*result, "preview_telemetry", sbs_preview_serialize_telemetry(server->preview));
    }
    if (server->audio) {
        cJSON_AddItemToObject(*result, "audio", sbs_audio_mixer_serialize_state(server->audio));
    }
    cJSON_AddItemToObject(*result, "runtime", serialize_runtime_health(server));

    {
        bool restart_required = pending_canvas_differs(server);
        if (restart_required) {
            cJSON_AddItemToObject(*result, "pending_canvas",
                                  duplicate_pending_canvas_object(server));
        }
        cJSON_AddBoolToObject(*result, "canvas_restart_required", restart_required);
    }

    if (server->comp_thread) {
        sbs_comp_timing_stats_t timing;
        sbs_compositor_thread_get_timing(server->comp_thread, &timing);
        cJSON *comp = cJSON_CreateObject();
        cJSON_AddNumberToObject(comp, "frame_count", (double)timing.frame_count);
        cJSON_AddNumberToObject(comp, "content_frame_count", (double)timing.content_frame_count);
        cJSON_AddNumberToObject(comp, "repeated_frame_count", (double)timing.repeated_frame_count);
        cJSON_AddNumberToObject(comp, "frames_dropped", (double)timing.frames_dropped);
        cJSON_AddNumberToObject(comp, "last_frame_time_ms", timing.last_frame_time_ms);
        cJSON_AddNumberToObject(comp, "avg_frame_time_ms", timing.avg_frame_time_ms);
        cJSON_AddNumberToObject(comp, "min_frame_time_ms", timing.min_frame_time_ms);
        cJSON_AddNumberToObject(comp, "max_frame_time_ms", timing.max_frame_time_ms);
        cJSON_AddNumberToObject(comp, "fps_actual", server->last_telemetry_monotonic_usec > 0
            ? server->last_telemetry_compositor_fps
            : timing.fps_actual);
        cJSON_AddNumberToObject(comp, "content_fps", server->last_telemetry_monotonic_usec > 0
            ? server->last_telemetry_content_fps
            : 0.0);
        cJSON_AddItemToObject(*result, "compositor_timing", comp);

        cJSON *mailboxes = cJSON_CreateObject();
        sbs_native_canvas_mailbox_stats_t mb;
        sbs_compositor_thread_native_mailbox_get_stats(
            server->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_PREVIEW, &mb);
        add_native_mailbox_stats(mailboxes, "preview", &mb);
        sbs_compositor_thread_native_mailbox_get_stats(
            server->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_OUTPUT, &mb);
        add_native_mailbox_stats(mailboxes, "output", &mb);
        sbs_compositor_thread_native_mailbox_get_stats(
            server->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_SNAPSHOT, &mb);
        add_native_mailbox_stats(mailboxes, "snapshot", &mb);
        cJSON_AddItemToObject(*result, "native_mailboxes", mailboxes);
    }

    /* Include telemetry so controller-proxied clients (which don't receive
     * pubsub events) can display metrics via the polling path. */
    {
        uint64_t distributed = 0;
        uint32_t frames_dropped = 0;
        uint64_t frame_count = 0;
        uint64_t content_frame_count = 0;
        double compositor_fps = 0.0;
        double content_fps = 0.0;
        double target_fps = 0.0;
        double bitrate_kbps = 0.0;
        bool have_cached_telemetry = false;
        bool pipeline_slow = false;
        int64_t now_usec = g_get_monotonic_time();

        if (server->output_router) {
            distributed = sbs_output_router_frames_distributed(server->output_router);
        }
        if (server->comp_thread) {
            sbs_comp_timing_stats_t timing;
            sbs_compositor_thread_get_timing(server->comp_thread, &timing);
            frame_count = timing.frame_count;
            content_frame_count = timing.content_frame_count;
            frames_dropped = timing.frames_dropped;
        }
        if (server->scene_graph && server->scene_graph->canvas.fps_den > 0) {
            target_fps = (double)server->scene_graph->canvas.fps_num /
                         (double)server->scene_graph->canvas.fps_den;
        }
        if (server->last_telemetry_monotonic_usec > 0) {
            compositor_fps = server->last_telemetry_compositor_fps;
            content_fps = server->last_telemetry_content_fps;
            pipeline_slow = server->last_telemetry_pipeline_slow;
            have_cached_telemetry = true;
        }
        if (!have_cached_telemetry && server->started_monotonic_usec > 0 && now_usec > server->started_monotonic_usec) {
            double uptime_sec = (double)(now_usec - server->started_monotonic_usec) / 1000000.0;
            if (uptime_sec > 0.0) {
                compositor_fps = (double)frame_count / uptime_sec;
                content_fps = (double)content_frame_count / uptime_sec;
            }
        }
        if (frame_count > 0) {
            bitrate_kbps = (double)(distributed * 1920ULL * 1080ULL * 12ULL) / 1000.0 / (double)frame_count;
        }

        cJSON *telemetry = cJSON_CreateObject();
        cJSON_AddNumberToObject(telemetry, "compositor_fps", compositor_fps);
        cJSON_AddNumberToObject(telemetry, "content_fps", content_fps);
        cJSON_AddNumberToObject(telemetry, "frames_rendered", (double)frame_count);
        cJSON_AddNumberToObject(telemetry, "content_frames", (double)content_frame_count);
        cJSON_AddNumberToObject(telemetry, "bitrate_kbps", bitrate_kbps);
        cJSON_AddNumberToObject(telemetry, "latency_ms", frames_dropped > 0 ? 90.0 : 40.0);
        cJSON_AddNumberToObject(telemetry, "cpu_usage", read_cpu_usage());
        cJSON_AddNumberToObject(telemetry, "gpu_usage", 0.0);
        if (!have_cached_telemetry) {
            pipeline_slow = frames_dropped > 10 ||
                (target_fps > 0.0 && compositor_fps > 0.0 && compositor_fps + 0.5 < target_fps) ||
                (target_fps > 0.0 && content_frame_count > 1 && content_fps + 0.5 < target_fps);
        }
        cJSON_AddBoolToObject(telemetry, "pipeline_slow", pipeline_slow);
        cJSON_AddItemToObject(*result, "telemetry", telemetry);
    }

    return SBS_OK;
}

int sbs_api_handle_system_get_info(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    (void)server;
    (void)client;
    (void)params;
    (void)error;
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "version", SBS_VERSION_STRING);
    cJSON_AddNumberToObject(*result, "protocol_version", 1);
    cJSON_AddStringToObject(*result, "platform", SBS_PLATFORM);
    cJSON_AddStringToObject(*result, "hostname", g_get_host_name());
    cJSON_AddItemToObject(*result, "runtime", serialize_runtime_health(server));
    return SBS_OK;
}
