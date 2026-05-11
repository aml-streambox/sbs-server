#define SBS_LOG_COMP "api-preview"

#include "sbs/api_server.h"
#include "sbs/log.h"

#include <string.h>

static gboolean token_has_suffix(const char *token, size_t len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);
    return len >= suffix_len && memcmp(token + len - suffix_len, suffix, suffix_len) == 0;
}

static char *rewrite_mdns_tokens_to_peer_ip(const char *text, const char *peer_ip)
{
    GString *out;
    const char *p;
    gboolean changed = FALSE;

    if (!text || !peer_ip || !*peer_ip || !strstr(text, ".local")) {
        return NULL;
    }

    out = g_string_sized_new(strlen(text));
    p = text;
    while (*p) {
        const char *start;
        size_t len;

        if (g_ascii_isspace(*p)) {
            g_string_append_c(out, *p++);
            continue;
        }

        start = p;
        while (*p && !g_ascii_isspace(*p)) {
            p++;
        }
        len = (size_t)(p - start);

        if (token_has_suffix(start, len, ".local")) {
            g_string_append(out, peer_ip);
            changed = TRUE;
        } else {
            g_string_append_len(out, start, len);
        }
    }

    if (!changed) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

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

static const char *webrtc_peer_ip(cJSON *params, sbs_api_client_t *client)
{
    const char *forwarded_peer_ip = json_str(params, "__client_peer_ip");
    if (forwarded_peer_ip && *forwarded_peer_ip) {
        return forwarded_peer_ip;
    }
    return client ? client->peer_ip : NULL;
}

static void publish_profile_event(sbs_api_server_t *server, const char *topic, const sbs_preview_profile_t *profile)
{
    sbs_api_server_publish(server, topic, sbs_preview_serialize_profile(profile));
}

int sbs_api_handle_preview_list_profiles(sbs_api_server_t *server, sbs_api_client_t *client,
                                         cJSON *params, cJSON **result, cJSON **error)
{
    (void)client;
    (void)params;
    (void)error;
    *result = sbs_preview_serialize_profile_catalog(server->preview);
    return SBS_OK;
}

int sbs_api_handle_preview_ensure_profile(sbs_api_server_t *server, sbs_api_client_t *client,
                                          cJSON *params, cJSON **result, cJSON **error)
{
    sbs_preview_profile_t *profile = NULL;
    int rc;
    (void)client;
    rc = sbs_preview_engine_ensure_profile(server->preview, json_str(params, "profile_id"), &profile);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to ensure preview profile");
        return rc;
    }
    publish_profile_event(server, "preview.profile.active", profile);
    *result = sbs_preview_serialize_profile(profile);
    return SBS_OK;
}

int sbs_api_handle_preview_get_status(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    sbs_preview_profile_t *profile;
    (void)client;
    (void)error;

    if (profile_id) {
        profile = sbs_preview_engine_get_profile(server->preview, profile_id);
        if (!profile) {
            *error = api_error(-32001, "Preview profile not found");
            return SBS_ERR_NOT_FOUND;
        }
        *result = sbs_preview_serialize_profile(profile);
        return SBS_OK;
    }

    *result = cJSON_CreateObject();
    cJSON_AddItemToObject(*result, "catalog", sbs_preview_serialize_profile_catalog(server->preview));
    cJSON_AddItemToObject(*result, "telemetry", sbs_preview_serialize_telemetry(server->preview));
    return SBS_OK;
}

int sbs_api_handle_preview_release_profile(sbs_api_server_t *server, sbs_api_client_t *client,
                                           cJSON *params, cJSON **result, cJSON **error)
{
    sbs_preview_profile_t *profile = NULL;
    int rc;
    (void)client;
    rc = sbs_preview_engine_release_profile(server->preview, json_str(params, "profile_id"), &profile);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Preview profile not found");
        return rc;
    }
    publish_profile_event(server, "preview.profile.released", profile);
    *result = sbs_preview_serialize_profile(profile);
    return SBS_OK;
}

/* ── WebRTC signaling handlers ────────────────────────────────── */

int sbs_api_handle_preview_webrtc_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    sbs_preview_profile_t *profile = NULL;
    const char *offer_sdp = NULL;
    cJSON *candidates = NULL;
    int rc;
    (void)client;

    if (!profile_id) profile_id = "preview-h264-webrtc";

    /* Run ensure_profile + offer generation on the main loop thread.
     * This function blocks until the offer is ready or times out. */
    rc = sbs_preview_engine_webrtc_start(server->preview, profile_id,
                                         &profile, &offer_sdp, &candidates);
    if (rc != SBS_OK || !offer_sdp) {
        if (candidates) cJSON_Delete(candidates);
        if (rc == SBS_ERR_INVAL || rc == SBS_ERR_NOT_FOUND) {
            *error = api_error(-32010, "WebRTC preview profile unavailable");
        } else {
            *error = api_error(-32010, "WebRTC offer not available (timeout)");
        }
        return rc != SBS_OK ? rc : SBS_ERR_IO;
    }

    if (profile) {
        publish_profile_event(server, "preview.profile.active", profile);
    }

    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "profile_id", profile_id);
    cJSON_AddStringToObject(*result, "type", "offer");
    cJSON_AddStringToObject(*result, "sdp", offer_sdp);
    if (candidates) {
        cJSON_AddItemToObject(*result, "iceCandidates", candidates);
    }
    return SBS_OK;
}

int sbs_api_handle_preview_webrtc_answer(sbs_api_server_t *server, sbs_api_client_t *client,
                                         cJSON *params, cJSON **result, cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    const char *sdp = json_str(params, "sdp");
    char *rewritten_sdp = NULL;
    const char *answer_sdp;
    int rc;

    if (!profile_id) profile_id = "preview-h264-webrtc";
    if (!sdp) {
        *error = api_error(-32602, "Missing 'sdp' parameter");
        return SBS_ERR_INVAL;
    }

    const char *peer_ip = webrtc_peer_ip(params, client);
    rewritten_sdp = rewrite_mdns_tokens_to_peer_ip(sdp, peer_ip);
    answer_sdp = rewritten_sdp ? rewritten_sdp : sdp;
    if (rewritten_sdp) {
        LOG_I("WebRTC: rewrote mDNS answer candidate(s) to peer IP %s", peer_ip);
    }

    rc = sbs_preview_engine_set_webrtc_answer(server->preview, profile_id, answer_sdp);
    g_free(rewritten_sdp);
    if (rc != SBS_OK) {
        *error = api_error(-32010, "Failed to set WebRTC answer");
        return rc;
    }

    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", true);
    return SBS_OK;
}

int sbs_api_handle_preview_webrtc_ice(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    const char *candidate = json_str(params, "candidate");
    char *rewritten_candidate = NULL;
    const char *candidate_text;
    cJSON *mline_item;
    unsigned int mline_index;
    int rc;

    if (!profile_id) profile_id = "preview-h264-webrtc";
    if (!candidate) {
        *error = api_error(-32602, "Missing 'candidate' parameter");
        return SBS_ERR_INVAL;
    }

    mline_item = cJSON_GetObjectItemCaseSensitive(params, "sdpMLineIndex");
    mline_index = cJSON_IsNumber(mline_item) ? (unsigned int)mline_item->valueint : 0;

    const char *peer_ip = webrtc_peer_ip(params, client);
    rewritten_candidate = rewrite_mdns_tokens_to_peer_ip(candidate, peer_ip);
    candidate_text = rewritten_candidate ? rewritten_candidate : candidate;
    if (rewritten_candidate) {
        LOG_I("WebRTC: rewrote mDNS ICE candidate to peer IP %s", peer_ip);
    }

    rc = sbs_preview_engine_add_webrtc_ice(server->preview, profile_id, mline_index, candidate_text);
    g_free(rewritten_candidate);
    if (rc != SBS_OK) {
        *error = api_error(-32010, "Failed to add ICE candidate");
        return rc;
    }

    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", true);
    return SBS_OK;
}

/* ── Preview encoder config handlers ─────────────────────────── */

int sbs_api_handle_preview_get_encoder_config(sbs_api_server_t *server,
                                               sbs_api_client_t *client,
                                               cJSON *params,
                                               cJSON **result,
                                               cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    sbs_preview_profile_t *profile;
    (void)client;

    if (!profile_id) profile_id = "preview-h264-webrtc";

    profile = sbs_preview_engine_get_profile(server->preview, profile_id);
    if (!profile) {
        *error = api_error(-32001, "Preview profile not found");
        return SBS_ERR_NOT_FOUND;
    }

    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "profile_id", profile->id);
    cJSON_AddNumberToObject(*result, "width", profile->width);
    cJSON_AddNumberToObject(*result, "height", profile->height);
    cJSON_AddNumberToObject(*result, "downscale_factor", profile->downscale_factor);
    cJSON_AddNumberToObject(*result, "framerate", profile->framerate);
    cJSON_AddNumberToObject(*result, "bitrate_kbps", profile->bitrate_kbps);
    cJSON_AddStringToObject(*result, "codec", profile->codec ? profile->codec : "h264");
    cJSON_AddBoolToObject(*result, "active", profile->active);
    return SBS_OK;
}

int sbs_api_handle_preview_update_encoder_config(sbs_api_server_t *server,
                                                  sbs_api_client_t *client,
                                                  cJSON *params,
                                                  cJSON **result,
                                                  cJSON **error)
{
    const char *profile_id = json_str(params, "profile_id");
    cJSON *width_j, *height_j, *downscale_j, *fps_j, *bitrate_j;
    uint32_t downscale_factor = 0, framerate = 0, bitrate_kbps = 0;
    int rc;
    (void)client;

    if (!profile_id) profile_id = "preview-h264-webrtc";

    width_j = cJSON_GetObjectItemCaseSensitive(params, "width");
    height_j = cJSON_GetObjectItemCaseSensitive(params, "height");
    if (width_j || height_j) {
        *error = api_error(-32602, "Preview resolution is derived from canvas; use downscale_factor 1, 2, 4, or 8");
        return SBS_ERR_INVAL;
    }

    downscale_j = cJSON_GetObjectItemCaseSensitive(params, "downscale_factor");
    if (downscale_j && cJSON_IsNumber(downscale_j))
        downscale_factor = (uint32_t)downscale_j->valuedouble;

    fps_j = cJSON_GetObjectItemCaseSensitive(params, "framerate");
    if (fps_j && cJSON_IsNumber(fps_j))
        framerate = (uint32_t)fps_j->valuedouble;

    bitrate_j = cJSON_GetObjectItemCaseSensitive(params, "bitrate_kbps");
    if (bitrate_j && cJSON_IsNumber(bitrate_j))
        bitrate_kbps = (uint32_t)bitrate_j->valuedouble;

    rc = sbs_preview_engine_update_profile_config(server->preview, profile_id,
                                                    downscale_factor, framerate, bitrate_kbps);
    if (rc != SBS_OK) {
        *error = api_error(-32011, "Failed to update preview encoder config");
        return rc;
    }

    /* Return updated config */
    return sbs_api_handle_preview_get_encoder_config(server, client, params, result, error);
}
