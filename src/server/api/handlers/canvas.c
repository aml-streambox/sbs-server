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

static sbs_export_color_mode_t export_color_mode_from_pixel_format(sbs_pixel_format_t pixel_format)
{
    return pixel_format == SBS_PIXEL_FORMAT_P010
        ? SBS_EXPORT_COLOR_HDR10
        : SBS_EXPORT_COLOR_SDR;
}

static bool resolve_canvas_color_fields(const char *pixel_format_str,
                                        const char *colorimetry_str,
                                        const char *legacy_color_mode,
                                        sbs_pixel_format_t base_pixel_format,
                                        sbs_colorimetry_t base_colorimetry,
                                        sbs_pixel_format_t *pixel_format_out,
                                        sbs_colorimetry_t *colorimetry_out)
{
    sbs_pixel_format_t pixel_format = base_pixel_format;
    sbs_colorimetry_t colorimetry = base_colorimetry;

    if (pixel_format_str) {
        if (!sbs_pixel_format_parse(pixel_format_str, &pixel_format))
            return false;
    } else if (legacy_color_mode) {
        pixel_format = sbs_pixel_format_from_legacy_color_mode(legacy_color_mode);
    }

    if (colorimetry_str) {
        if (!sbs_colorimetry_parse(colorimetry_str, &colorimetry))
            return false;
    } else if (legacy_color_mode) {
        colorimetry = sbs_colorimetry_from_legacy_color_mode(legacy_color_mode);
    }

    if (pixel_format_out)
        *pixel_format_out = pixel_format;
    if (colorimetry_out)
        *colorimetry_out = colorimetry;
    return true;
}

static void add_canvas_json(cJSON *obj,
                            uint32_t width,
                            uint32_t height,
                            uint32_t fps_num,
                            uint32_t fps_den,
                            const char *pixel_format,
                            const char *colorimetry,
                            const char *background_color)
{
    cJSON_AddNumberToObject(obj, "width", width);
    cJSON_AddNumberToObject(obj, "height", height);
    cJSON_AddNumberToObject(obj, "fps_num", fps_num);
    cJSON_AddNumberToObject(obj, "fps_den", fps_den);
    cJSON_AddStringToObject(obj, "pixel_format", pixel_format ? pixel_format : "nv21");
    cJSON_AddStringToObject(obj, "colorimetry", colorimetry ? colorimetry : "sdr");
    cJSON_AddStringToObject(obj, "color_mode",
                            g_strcmp0(pixel_format, "p010") == 0 ? "hdr10" : "sdr");
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
                    sbs_pixel_format_name(graph->canvas.pixel_format),
                    sbs_colorimetry_name(graph->canvas.colorimetry),
                    graph->canvas.background_color);
    cJSON_AddItemToObject(result, "canvas", canvas);
}

static void add_pending_canvas_result(cJSON *result,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t fps_num,
                                       uint32_t fps_den,
                                       const char *pixel_format,
                                       const char *colorimetry,
                                       const char *background_color)
{
    cJSON *canvas = cJSON_CreateObject();
    add_canvas_json(canvas, width, height, fps_num, fps_den,
                    pixel_format, colorimetry, background_color);
    cJSON_AddItemToObject(result, "pending_canvas", canvas);
}

static void set_server_pending_canvas(sbs_api_server_t *server,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t fps_num,
                                      uint32_t fps_den,
                                      const char *pixel_format,
                                      const char *colorimetry,
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
    g_free(server->pending_canvas_pixel_format);
    g_free(server->pending_canvas_colorimetry);
    g_free(server->pending_canvas_background_color);
    server->pending_canvas_pixel_format = g_strdup(pixel_format ? pixel_format : "nv21");
    server->pending_canvas_colorimetry = g_strdup(colorimetry ? colorimetry : "sdr");
    server->pending_canvas_color_mode = g_strdup(
        g_strcmp0(server->pending_canvas_pixel_format, "p010") == 0 ? "hdr10" : "sdr");
    server->pending_canvas_background_color = g_strdup(background_color ? background_color : "#000000");
}

static void clear_server_pending_canvas(sbs_api_server_t *server)
{
    if (!server) {
        return;
    }

    server->pending_canvas_valid = false;
    g_clear_pointer(&server->pending_canvas_color_mode, g_free);
    g_clear_pointer(&server->pending_canvas_pixel_format, g_free);
    g_clear_pointer(&server->pending_canvas_colorimetry, g_free);
    g_clear_pointer(&server->pending_canvas_background_color, g_free);
}

static bool saved_canvas_requires_restart(sbs_api_server_t *server,
                                          cJSON *state_obj,
                                          uint32_t *width_out,
                                          uint32_t *height_out,
                                          uint32_t *fps_num_out,
                                          uint32_t *fps_den_out,
                                          const char **pixel_format_out,
                                          const char **colorimetry_out,
                                          const char **background_color_out)
{
    cJSON *canvas = state_obj ? cJSON_GetObjectItemCaseSensitive(state_obj, "canvas") : NULL;
    uint32_t desired_w;
    uint32_t desired_h;
    uint32_t desired_fps_num;
    uint32_t desired_fps_den;
    sbs_pixel_format_t desired_pixel_format;
    sbs_colorimetry_t desired_colorimetry;
    const char *desired_pf;
    const char *desired_ci;
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
    resolve_canvas_color_fields(json_str(canvas, "pixel_format"),
                                json_str(canvas, "colorimetry"),
                                json_str(canvas, "color_mode"),
                                server->scene_graph->canvas.pixel_format,
                                server->scene_graph->canvas.colorimetry,
                                &desired_pixel_format,
                                &desired_colorimetry);
    desired_pf = sbs_pixel_format_name(desired_pixel_format);
    desired_ci = sbs_colorimetry_name(desired_colorimetry);
    desired_bg = json_str(canvas, "background_color");
    if (!desired_bg)
        desired_bg = server->scene_graph->canvas.background_color;

    if (width_out) *width_out = desired_w;
    if (height_out) *height_out = desired_h;
    if (fps_num_out) *fps_num_out = desired_fps_num;
    if (fps_den_out) *fps_den_out = desired_fps_den;
    if (pixel_format_out) *pixel_format_out = desired_pf;
    if (colorimetry_out) *colorimetry_out = desired_ci;
    if (background_color_out) *background_color_out = desired_bg;

    if (!server->comp_thread)
        return false;

    sbs_compositor_thread_get_canvas_config(server->comp_thread,
                                            &comp_w, &comp_h, &comp_fps,
                                            &comp_color);
    desired_color = export_color_mode_from_pixel_format(desired_pixel_format);

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

    if (!item) {
        cJSON_AddStringToObject(canvas, key, value ? value : "");
        return;
    }
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

    uint32_t base_w = server->pending_canvas_valid ? server->pending_canvas_width : graph->canvas.width;
    uint32_t base_h = server->pending_canvas_valid ? server->pending_canvas_height : graph->canvas.height;
    uint32_t base_fps_num = server->pending_canvas_valid ? server->pending_canvas_fps_num : graph->canvas.fps_num;
    uint32_t base_fps_den = server->pending_canvas_valid ? server->pending_canvas_fps_den : graph->canvas.fps_den;
    sbs_pixel_format_t base_pixel_format = graph->canvas.pixel_format;
    sbs_colorimetry_t base_colorimetry = graph->canvas.colorimetry;
    sbs_pixel_format_t resolved_pixel_format;
    sbs_colorimetry_t resolved_colorimetry;
    const char *resolved_pf;
    const char *resolved_ci;
    const char *base_bg = server->pending_canvas_valid && server->pending_canvas_background_color
        ? server->pending_canvas_background_color
        : graph->canvas.background_color;
    uint32_t new_w = (uint32_t)json_num(canvas_obj, "width", base_w);
    uint32_t new_h = (uint32_t)json_num(canvas_obj, "height", base_h);
    uint32_t new_fps_num = (uint32_t)json_num(canvas_obj, "fps_num", base_fps_num);
    uint32_t new_fps_den = (uint32_t)json_num(canvas_obj, "fps_den", base_fps_den);
    cJSON *width_item = cJSON_GetObjectItemCaseSensitive(canvas_obj, "width");
    cJSON *height_item = cJSON_GetObjectItemCaseSensitive(canvas_obj, "height");
    const char *new_bg = json_str(canvas_obj, "background_color");
    const char *new_cm = json_str(canvas_obj, "color_mode");
    const char *new_pf = json_str(canvas_obj, "pixel_format");
    const char *new_ci = json_str(canvas_obj, "colorimetry");
    const char *resolved_bg = new_bg ? new_bg : base_bg;
    bool resolution_changed = cJSON_IsNumber(width_item) || cJSON_IsNumber(height_item);

    if (server->pending_canvas_valid && server->pending_canvas_pixel_format)
        sbs_pixel_format_parse(server->pending_canvas_pixel_format, &base_pixel_format);
    if (server->pending_canvas_valid && server->pending_canvas_colorimetry)
        sbs_colorimetry_parse(server->pending_canvas_colorimetry, &base_colorimetry);

    if (!resolve_canvas_color_fields(new_pf, new_ci, new_cm,
                                     base_pixel_format, base_colorimetry,
                                     &resolved_pixel_format,
                                     &resolved_colorimetry)) {
        *error = api_error(-32602, "Invalid canvas pixel_format or colorimetry");
        return SBS_ERR_INVAL;
    }
    resolved_pf = sbs_pixel_format_name(resolved_pixel_format);
    resolved_ci = sbs_colorimetry_name(resolved_colorimetry);

    if (!new_fps_den) {
        new_fps_den = 1;
    }
    if (resolution_changed &&
        (new_w == 0 || new_h == 0 || (new_w & 1u) != 0 || (new_h & 1u) != 0)) {
        *error = api_error(-32602, "Canvas width and height must be positive even numbers");
        return SBS_ERR_INVAL;
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
    set_canvas_string_field(bundle_canvas, "pixel_format", resolved_pf);
    set_canvas_string_field(bundle_canvas, "colorimetry", resolved_ci);
    set_canvas_string_field(bundle_canvas, "color_mode",
                            sbs_legacy_color_mode_for_pixel_format(resolved_pixel_format));
    set_canvas_string_field(bundle_canvas, "background_color", resolved_bg);

    if (sbs_config_manager_save_bundle(server->config, bundle) != SBS_OK) {
        cJSON_Delete(bundle);
        *error = api_error(-32002, "Failed to save canvas settings");
        return SBS_ERR_IO;
    }
    cJSON_Delete(bundle);

    LOG_I("saved canvas settings; waiting for apply: %ux%u@%u/%u pixel_format=%s colorimetry=%s",
          new_w, new_h, new_fps_num, new_fps_den, resolved_pf, resolved_ci);
    set_server_pending_canvas(server, new_w, new_h, new_fps_num, new_fps_den,
                              resolved_pf, resolved_ci, resolved_bg);

    *result = cJSON_CreateObject();
    add_running_canvas_result(*result, graph);
    add_pending_canvas_result(*result, new_w, new_h, new_fps_num, new_fps_den,
                              resolved_pf, resolved_ci, resolved_bg);
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
    const char *pending_pf = NULL;
    const char *pending_ci = NULL;
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
                                      &pending_pf, &pending_ci, &pending_bg)) {
        LOG_I("saved canvas requires compositor restart; applying supervised restart: %ux%u@%u/%u pixel_format=%s colorimetry=%s",
              pending_w, pending_h, pending_fps_num, pending_fps_den,
              pending_pf ? pending_pf : "nv21",
              pending_ci ? pending_ci : "sdr");
        set_server_pending_canvas(server, pending_w, pending_h,
                                  pending_fps_num, pending_fps_den,
                                  pending_pf ? pending_pf : "nv21",
                                  pending_ci ? pending_ci : "sdr",
                                  pending_bg ? pending_bg : "#000000");
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
