#define SBS_LOG_COMP "api-encoder"

#include "sbs/api_server.h"
#include "sbs/encoder_manager.h"
#include "sbs/log.h"

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static const char *gop_preset_from_pattern(int32_t gop_pattern)
{
    switch (gop_pattern) {
    case 0:
        return "low_delay";
    case 1:
    case 2:
        return "b_frames";
    default:
        return "custom";
    }
}

static bool gop_pattern_from_preset(const char *preset, int32_t *out_pattern)
{
    if (!preset || !out_pattern)
        return false;
    if (g_strcmp0(preset, "low_delay") == 0 ||
        g_strcmp0(preset, "no_b_frames") == 0 ||
        g_strcmp0(preset, "ip") == 0) {
        *out_pattern = 0;
        return true;
    }
    if (g_strcmp0(preset, "b_frames") == 0 ||
        g_strcmp0(preset, "ibp") == 0 ||
        g_strcmp0(preset, "balanced") == 0) {
        *out_pattern = 2;
        return true;
    }
    if (g_strcmp0(preset, "custom") == 0) {
        return false;
    }
    return false;
}

int sbs_api_handle_encoder_get_config(sbs_api_server_t *server,
                                       sbs_api_client_t *client,
                                       cJSON *params,
                                       cJSON **result,
                                       cJSON **error)
{
    (void)client;
    (void)params;

    if (!server->encoder_mgr) {
        *error = api_error(-32010, "Encoder manager not available");
        return -1;
    }

    sbs_encoder_config_t cfg = {0};
    sbs_encoder_manager_get_config(server->encoder_mgr, &cfg);

    uint32_t width = 0, height = 0, fps_num = 0, fps_den = 0;
    sbs_encoder_manager_get_format(server->encoder_mgr, &width, &height, &fps_num, &fps_den);

    sbs_encoder_manager_metrics_t metrics = {0};
    sbs_encoder_manager_get_metrics(server->encoder_mgr, &metrics);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "codec", cfg.codec ? cfg.codec : "h265");
    cJSON_AddNumberToObject(obj, "bitrate_kbps", cfg.bitrate_kbps);
    cJSON_AddNumberToObject(obj, "gop_size", cfg.gop_size);
    cJSON_AddNumberToObject(obj, "keyframe_interval", cfg.gop_size);
    cJSON_AddNumberToObject(obj, "gop_pattern", cfg.gop_pattern);
    cJSON_AddStringToObject(obj, "gop_preset", gop_preset_from_pattern(cfg.gop_pattern));
    cJSON_AddBoolToObject(obj, "enable_b_frames", cfg.gop_pattern != 0);
    cJSON_AddNumberToObject(obj, "rc_mode", cfg.rc_mode);
    cJSON_AddStringToObject(obj, "pixel_format", sbs_pixel_format_name(cfg.pixel_format));
    cJSON_AddStringToObject(obj, "colorimetry", sbs_colorimetry_name(cfg.colorimetry));
    if (cfg.encoder)
        cJSON_AddStringToObject(obj, "encoder", cfg.encoder);
    cJSON_AddNumberToObject(obj, "width", width);
    cJSON_AddNumberToObject(obj, "height", height);
    cJSON_AddNumberToObject(obj, "fps_num", fps_num);
    cJSON_AddNumberToObject(obj, "fps_den", fps_den);
    cJSON_AddBoolToObject(obj, "active", metrics.pipeline_active);
    cJSON_AddNumberToObject(obj, "active_branches", metrics.active_branches);
    cJSON_AddNumberToObject(obj, "frames_pushed", (double)metrics.frames_pushed);

    *result = obj;
    return 0;
}

int sbs_api_handle_encoder_update_config(sbs_api_server_t *server,
                                          sbs_api_client_t *client,
                                          cJSON *params,
                                          cJSON **result,
                                          cJSON **error)
{
    (void)client;

    if (!server->encoder_mgr) {
        *error = api_error(-32010, "Encoder manager not available");
        return -1;
    }

    /* Get current config as defaults */
    sbs_encoder_config_t cfg = {0};
    sbs_encoder_manager_get_config(server->encoder_mgr, &cfg);

    /* Apply overrides from params */
    cJSON *codec_j = cJSON_GetObjectItemCaseSensitive(params, "codec");
    if (codec_j && cJSON_IsString(codec_j))
        cfg.codec = cJSON_GetStringValue(codec_j);

    cJSON *bitrate_j = cJSON_GetObjectItemCaseSensitive(params, "bitrate_kbps");
    if (bitrate_j && cJSON_IsNumber(bitrate_j))
        cfg.bitrate_kbps = (uint32_t)bitrate_j->valuedouble;

    cJSON *gop_j = cJSON_GetObjectItemCaseSensitive(params, "gop_size");
    if (gop_j && cJSON_IsNumber(gop_j))
        cfg.gop_size = (uint32_t)gop_j->valuedouble;

    cJSON *key_interval_j = cJSON_GetObjectItemCaseSensitive(params, "keyframe_interval");
    if (key_interval_j && cJSON_IsNumber(key_interval_j))
        cfg.gop_size = (uint32_t)key_interval_j->valuedouble;

    cJSON *gop_pattern_j = cJSON_GetObjectItemCaseSensitive(params, "gop_pattern");
    if (gop_pattern_j && cJSON_IsNumber(gop_pattern_j))
        cfg.gop_pattern = (int32_t)gop_pattern_j->valuedouble;

    cJSON *gop_preset_j = cJSON_GetObjectItemCaseSensitive(params, "gop_preset");
    if (gop_preset_j && cJSON_IsString(gop_preset_j)) {
        int32_t preset_pattern = cfg.gop_pattern;
        if (gop_pattern_from_preset(cJSON_GetStringValue(gop_preset_j), &preset_pattern))
            cfg.gop_pattern = preset_pattern;
    }

    cJSON *enable_b_frames_j = cJSON_GetObjectItemCaseSensitive(params, "enable_b_frames");
    if (enable_b_frames_j && cJSON_IsBool(enable_b_frames_j))
        cfg.gop_pattern = cJSON_IsTrue(enable_b_frames_j) ? 2 : 0;

    cJSON *rc_mode_j = cJSON_GetObjectItemCaseSensitive(params, "rc_mode");
    if (rc_mode_j && cJSON_IsNumber(rc_mode_j))
        cfg.rc_mode = (int32_t)rc_mode_j->valuedouble;

    cJSON *pixel_format_j = cJSON_GetObjectItemCaseSensitive(params, "pixel_format");
    if (pixel_format_j && cJSON_IsString(pixel_format_j) &&
        !sbs_pixel_format_parse(cJSON_GetStringValue(pixel_format_j), &cfg.pixel_format)) {
        *error = api_error(-32602, "Invalid encoder pixel_format");
        return SBS_ERR_INVAL;
    }

    cJSON *colorimetry_j = cJSON_GetObjectItemCaseSensitive(params, "colorimetry");
    if (colorimetry_j && cJSON_IsString(colorimetry_j) &&
        !sbs_colorimetry_parse(cJSON_GetStringValue(colorimetry_j), &cfg.colorimetry)) {
        *error = api_error(-32602, "Invalid encoder colorimetry");
        return SBS_ERR_INVAL;
    }

    char *log_codec = g_strdup(cfg.codec ? cfg.codec : "h265");
    int rc = sbs_encoder_manager_update_config(server->encoder_mgr, &cfg);
    if (rc != 0) {
        g_free(log_codec);
        *error = api_error(-32011, "Failed to update encoder config");
        return rc;
    }

    LOG_I("encoder config updated: codec=%s bitrate=%u gop=%u gop_pattern=%d rc_mode=%d pixel_format=%s colorimetry=%s",
          log_codec, cfg.bitrate_kbps, cfg.gop_size, cfg.gop_pattern, cfg.rc_mode,
          sbs_pixel_format_name(cfg.pixel_format),
          sbs_colorimetry_name(cfg.colorimetry));
    g_free(log_codec);

    /* Return new config */
    return sbs_api_handle_encoder_get_config(server, client, params, result, error);
}
