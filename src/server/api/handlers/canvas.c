#define SBS_LOG_COMP "api-canvas"

#include "sbs/api_server.h"
#include "sbs/log.h"

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

static void add_canvas_json(cJSON *obj,
                            uint32_t width,
                            uint32_t height,
                            uint32_t fps_num,
                            uint32_t fps_den,
                            const char *color_mode,
                            const char *background_color)
{
    cJSON_AddNumberToObject(obj, "width", width);
    cJSON_AddNumberToObject(obj, "height", height);
    cJSON_AddNumberToObject(obj, "fps_num", fps_num);
    cJSON_AddNumberToObject(obj, "fps_den", fps_den);
    cJSON_AddStringToObject(obj, "color_mode", color_mode);
    cJSON_AddStringToObject(obj, "background_color", background_color);
}

static void add_running_canvas_result(cJSON *result, const sbs_scene_graph_t *graph)
{
    cJSON *canvas = cJSON_CreateObject();
    add_canvas_json(canvas,
                    graph->canvas.width,
                    graph->canvas.height,
                    graph->canvas.fps_num,
                    graph->canvas.fps_den,
                    graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? "hdr10" : "sdr",
                    graph->canvas.background_color);
    cJSON_AddItemToObject(result, "canvas", canvas);
}

static void add_pending_canvas_result(cJSON *result,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t fps_num,
                                      uint32_t fps_den,
                                      const char *color_mode,
                                      const char *background_color)
{
    cJSON *canvas = cJSON_CreateObject();
    add_canvas_json(canvas, width, height, fps_num, fps_den, color_mode, background_color);
    cJSON_AddItemToObject(result, "pending_canvas", canvas);
}

static void set_server_pending_canvas(sbs_api_server_t *server,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t fps_num,
                                      uint32_t fps_den,
                                      const char *color_mode,
                                      const char *background_color)
{
    if (!server) {
        return;
    }

    server->pending_canvas_valid = true;
    server->pending_canvas_width = width;
    server->pending_canvas_height = height;
    server->pending_canvas_fps_num = fps_num;
    server->pending_canvas_fps_den = fps_den ? fps_den : 1;
    g_free(server->pending_canvas_color_mode);
    g_free(server->pending_canvas_background_color);
    server->pending_canvas_color_mode = g_strdup(color_mode ? color_mode : "sdr");
    server->pending_canvas_background_color = g_strdup(background_color ? background_color : "#000000");
}

static void clear_server_pending_canvas(sbs_api_server_t *server)
{
    if (!server) {
        return;
    }

    server->pending_canvas_valid = false;
    g_clear_pointer(&server->pending_canvas_color_mode, g_free);
    g_clear_pointer(&server->pending_canvas_background_color, g_free);
}

static bool saved_canvas_requires_restart(sbs_api_server_t *server,
                                          cJSON *state_obj,
                                          uint32_t *width_out,
                                          uint32_t *height_out,
                                          uint32_t *fps_num_out,
                                          uint32_t *fps_den_out,
                                          const char **color_mode_out,
                                          const char **background_color_out)
{
    cJSON *canvas = state_obj ? cJSON_GetObjectItemCaseSensitive(state_obj, "canvas") : NULL;
    uint32_t desired_w;
    uint32_t desired_h;
    uint32_t desired_fps_num;
    uint32_t desired_fps_den;
    const char *desired_cm;
    const char *desired_bg;
    uint32_t comp_w = 0;
    uint32_t comp_h = 0;
    uint32_t comp_fps = 0;
    sbs_export_color_mode_t comp_color = SBS_EXPORT_COLOR_SDR;
    sbs_export_color_mode_t desired_color;

    if (!server || !server->scene_graph || !cJSON_IsObject(canvas))
        return false;

    desired_w = (uint32_t)json_num(canvas, "width", server->scene_graph->canvas.width);
    desired_h = (uint32_t)json_num(canvas, "height", server->scene_graph->canvas.height);
    desired_fps_num = (uint32_t)json_num(canvas, "fps_num", server->scene_graph->canvas.fps_num);
    desired_fps_den = (uint32_t)json_num(canvas, "fps_den", server->scene_graph->canvas.fps_den);
    if (!desired_fps_den)
        desired_fps_den = 1;
    desired_cm = json_str(canvas, "color_mode");
    if (!desired_cm)
        desired_cm = server->scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? "hdr10" : "sdr";
    desired_bg = json_str(canvas, "background_color");
    if (!desired_bg)
        desired_bg = server->scene_graph->canvas.background_color;

    if (width_out) *width_out = desired_w;
    if (height_out) *height_out = desired_h;
    if (fps_num_out) *fps_num_out = desired_fps_num;
    if (fps_den_out) *fps_den_out = desired_fps_den;
    if (color_mode_out) *color_mode_out = desired_cm;
    if (background_color_out) *background_color_out = desired_bg;

    if (!server->comp_thread)
        return false;

    sbs_compositor_thread_get_canvas_config(server->comp_thread,
                                            &comp_w, &comp_h, &comp_fps,
                                            &comp_color);
    desired_color = g_strcmp0(desired_cm, "hdr10") == 0
        ? SBS_EXPORT_COLOR_HDR10 : SBS_EXPORT_COLOR_SDR;

    return desired_w != comp_w || desired_h != comp_h ||
           desired_fps_num != comp_fps || desired_fps_den != 1 ||
           desired_color != comp_color;
}

static void set_canvas_number_field(cJSON *canvas, const char *key, double value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(canvas, key);
    if (!cJSON_IsNumber(item)) {
        return;
    }
    item->valuedouble = value;
    item->valueint = (int)value;
}

static void set_canvas_string_field(cJSON *canvas, const char *key, const char *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(canvas, key);
    char *copy;

    if (!cJSON_IsString(item)) {
        return;
    }
    copy = strdup(value ? value : "");
    if (!copy) {
        return;
    }
    free(item->valuestring);
    item->valuestring = copy;
}

int sbs_api_handle_canvas_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_graph_t *graph;
    cJSON *canvas_obj;
    cJSON *bundle = NULL;
    cJSON *state_obj;
    cJSON *bundle_canvas;
    (void)client;

    if (!server || !server->scene_graph || !server->config) {
        *error = api_error(-32001, "Server not initialized");
        return SBS_ERR_INVAL;
    }

    graph = server->scene_graph;
    canvas_obj = cJSON_GetObjectItemCaseSensitive(params, "canvas");
    if (!canvas_obj) {
        *error = api_error(-32001, "canvas object is required");
        return SBS_ERR_INVAL;
    }

    uint32_t new_w = (uint32_t)json_num(canvas_obj, "width", graph->canvas.width);
    uint32_t new_h = (uint32_t)json_num(canvas_obj, "height", graph->canvas.height);
    uint32_t new_fps_num = (uint32_t)json_num(canvas_obj, "fps_num", graph->canvas.fps_num);
    uint32_t new_fps_den = (uint32_t)json_num(canvas_obj, "fps_den", graph->canvas.fps_den);
    const char *new_bg = json_str(canvas_obj, "background_color");
    const char *new_cm = json_str(canvas_obj, "color_mode");
    const char *resolved_cm = new_cm ? new_cm : (graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10 ? "hdr10" : "sdr");
    const char *resolved_bg = new_bg ? new_bg : graph->canvas.background_color;

    if (!new_fps_den) {
        new_fps_den = 1;
    }

    bundle = sbs_config_manager_build_bundle(server);
    if (!bundle) {
        *error = api_error(-32002, "Failed to build config bundle");
        return SBS_ERR_IO;
    }
    state_obj = cJSON_GetObjectItemCaseSensitive(bundle, "state");
    bundle_canvas = state_obj ? cJSON_GetObjectItemCaseSensitive(state_obj, "canvas") : NULL;
    if (!cJSON_IsObject(bundle_canvas)) {
        cJSON_Delete(bundle);
        *error = api_error(-32002, "Config bundle missing canvas state");
        return SBS_ERR_IO;
    }

    set_canvas_number_field(bundle_canvas, "width", new_w);
    set_canvas_number_field(bundle_canvas, "height", new_h);
    set_canvas_number_field(bundle_canvas, "fps_num", new_fps_num);
    set_canvas_number_field(bundle_canvas, "fps_den", new_fps_den);
    set_canvas_string_field(bundle_canvas, "color_mode", resolved_cm);
    set_canvas_string_field(bundle_canvas, "background_color", resolved_bg);

    if (sbs_config_manager_save_bundle(server->config, bundle) != SBS_OK) {
        cJSON_Delete(bundle);
        *error = api_error(-32002, "Failed to save canvas settings");
        return SBS_ERR_IO;
    }
    cJSON_Delete(bundle);

    LOG_I("saved canvas settings; waiting for apply: %ux%u@%u/%u color=%s",
          new_w, new_h, new_fps_num, new_fps_den, resolved_cm);
    set_server_pending_canvas(server, new_w, new_h, new_fps_num, new_fps_den,
                              resolved_cm, resolved_bg);

    *result = cJSON_CreateObject();
    add_running_canvas_result(*result, graph);
    add_pending_canvas_result(*result, new_w, new_h, new_fps_num, new_fps_den, resolved_cm, resolved_bg);
    cJSON_AddBoolToObject(*result, "restart_required", true);
    cJSON_AddBoolToObject(*result, "saved", true);
    return SBS_OK;
}

int sbs_api_handle_canvas_apply(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *bundle = NULL;
    cJSON *state_obj;
    uint32_t pending_w = 0;
    uint32_t pending_h = 0;
    uint32_t pending_fps_num = 0;
    uint32_t pending_fps_den = 1;
    const char *pending_cm = NULL;
    const char *pending_bg = NULL;
    (void)client;
    (void)params;

    if (!server || !server->scene_graph || !server->config) {
        *error = api_error(-32001, "Server not initialized");
        return SBS_ERR_INVAL;
    }

    if (sbs_config_manager_load_bundle(server->config, &bundle) != SBS_OK || !bundle) {
        *error = api_error(-32001, "No saved canvas settings to apply");
        return SBS_ERR_NOT_FOUND;
    }

    state_obj = cJSON_GetObjectItemCaseSensitive(bundle, "state");
    if (saved_canvas_requires_restart(server, state_obj,
                                      &pending_w, &pending_h,
                                      &pending_fps_num, &pending_fps_den,
                                      &pending_cm, &pending_bg)) {
        LOG_I("saved canvas requires compositor restart; leaving runtime unchanged: %ux%u@%u/%u color=%s",
              pending_w, pending_h, pending_fps_num, pending_fps_den,
              pending_cm ? pending_cm : "sdr");
        *result = cJSON_CreateObject();
        add_running_canvas_result(*result, server->scene_graph);
        add_pending_canvas_result(*result, pending_w, pending_h,
                                  pending_fps_num, pending_fps_den,
                                  pending_cm ? pending_cm : "sdr",
                                  pending_bg ? pending_bg : "#000000");
        cJSON_AddBoolToObject(*result, "applied", false);
        cJSON_AddBoolToObject(*result, "restart_required", true);
        set_server_pending_canvas(server, pending_w, pending_h,
                                  pending_fps_num, pending_fps_den,
                                  pending_cm ? pending_cm : "sdr",
                                  pending_bg ? pending_bg : "#000000");
        cJSON_Delete(bundle);
        return SBS_OK;
    }

    LOG_I("applying saved canvas settings via full runtime reinitialization");
    if (sbs_config_manager_apply_bundle(server->config, server, bundle) != SBS_OK) {
        cJSON_Delete(bundle);
        *error = api_error(-32002, "Failed to apply saved canvas settings");
        return SBS_ERR_IO;
    }
    cJSON_Delete(bundle);

    *result = cJSON_CreateObject();
    add_running_canvas_result(*result, server->scene_graph);
    cJSON_AddBoolToObject(*result, "applied", true);
    cJSON_AddBoolToObject(*result, "restart_required", false);
    clear_server_pending_canvas(server);
    return SBS_OK;
}
