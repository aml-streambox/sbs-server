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
    cJSON_AddNumberToObject(obj, "gop_pattern", cfg.gop_pattern);
    cJSON_AddNumberToObject(obj, "rc_mode", cfg.rc_mode);
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

    cJSON *gop_pattern_j = cJSON_GetObjectItemCaseSensitive(params, "gop_pattern");
    if (gop_pattern_j && cJSON_IsNumber(gop_pattern_j))
        cfg.gop_pattern = (int32_t)gop_pattern_j->valuedouble;

    cJSON *rc_mode_j = cJSON_GetObjectItemCaseSensitive(params, "rc_mode");
    if (rc_mode_j && cJSON_IsNumber(rc_mode_j))
        cfg.rc_mode = (int32_t)rc_mode_j->valuedouble;

    int rc = sbs_encoder_manager_update_config(server->encoder_mgr, &cfg);
    if (rc != 0) {
        *error = api_error(-32011, "Failed to update encoder config");
        return rc;
    }

    LOG_I("encoder config updated: codec=%s bitrate=%u gop=%u gop_pattern=%d rc_mode=%d",
          cfg.codec ? cfg.codec : "h265", cfg.bitrate_kbps, cfg.gop_size, cfg.gop_pattern, cfg.rc_mode);

    /* Return new config */
    return sbs_api_handle_encoder_get_config(server, client, params, result, error);
}
