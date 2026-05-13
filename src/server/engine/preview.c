#define SBS_LOG_COMP "preview"

#include "sbs/preview.h"
#include "sbs/direct_venc.h"
#include "sbs/log.h"
#include "sbs/types.h"

#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/mman.h>

#ifndef DRM_FORMAT_NV21
#define DRM_FORMAT_NV21 0x3132564e
#endif

#define SBS_PREVIEW_DEFAULT_DOWNSCALE_FACTOR 4u

#ifdef SBS_HAVE_GSTREAMER_PREVIEW
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/rtp/gstrtphdrext.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

#endif

typedef struct preview_runtime {
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *audio_appsrc;
    GstElement *webrtcbin;
    sbs_direct_venc_t *direct_enc;
    GMainContext *context;  /* dedicated context for webrtcbin processing */
    GMainLoop *loop;        /* main loop running on dedicated thread */
    GThread *thread;        /* dedicated thread for the WebRTC pipeline */
#endif
    uint64_t frames_pushed;
    uint64_t audio_buffers_pushed;
    bool audio_pts_origin_valid;
    uint64_t audio_pts_origin_ns;

    uint64_t first_pts_ns;  /* PTS of the first frame from the output router */
    char *directory;
    /* WebRTC signaling state */
    char *local_sdp;         /* SDP offer (owned) */
    GMutex ice_mutex;
    GPtrArray *ice_candidates; /* pending ICE candidates as cJSON strings */
    bool webrtc_ready;
    bool reference_color;
    sbs_preview_color_mode_t active_color_mode;
    GMutex ready_mutex;
    GCond ready_cond;
    sbs_preview_engine_t *engine; /* back-pointer for callbacks */
} preview_runtime_t;

struct sbs_preview_engine {
    GHashTable *profiles;
    GHashTable *runtimes;
    GMutex lock;
    uint16_t api_port;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t src_fps;
    sbs_preview_color_mode_t src_color_mode;
    uint64_t total_frames_produced;
    uint64_t total_frames_dropped;
    double latency_ms;
    bool degraded;
    bool webrtc_requestable;
    /* callback for publishing ICE candidates to WebSocket clients */
    sbs_preview_ice_callback_t ice_callback;
    void *ice_callback_userdata;
};

static void runtime_free(gpointer data)
{
    preview_runtime_t *runtime = data;
    if (!runtime) {
        return;
    }
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    if (runtime->loop) {
        g_main_loop_quit(runtime->loop);
    }
    if (runtime->thread) {
        g_thread_join(runtime->thread);
        runtime->thread = NULL;
    }
    if (runtime->appsrc) {
        gst_object_unref(runtime->appsrc);
    }
    if (runtime->audio_appsrc) {
        gst_object_unref(runtime->audio_appsrc);
    }
    if (runtime->webrtcbin) {
        gst_object_unref(runtime->webrtcbin);
    }
    if (runtime->direct_enc) {
        sbs_direct_venc_free(runtime->direct_enc);
    }
    if (runtime->pipeline) {
        gst_element_set_state(runtime->pipeline, GST_STATE_NULL);
        gst_object_unref(runtime->pipeline);
    }
    if (runtime->loop) {
        g_main_loop_unref(runtime->loop);
    }
    if (runtime->context) {
        g_main_context_unref(runtime->context);
    }
#endif
    g_free(runtime->directory);
    g_free(runtime->local_sdp);
    if (runtime->ice_candidates) {
        g_ptr_array_free(runtime->ice_candidates, TRUE);
    }
    g_mutex_clear(&runtime->ice_mutex);
    g_mutex_clear(&runtime->ready_mutex);
    g_cond_clear(&runtime->ready_cond);
    g_free(runtime);
}

#ifdef SBS_HAVE_GSTREAMER_PREVIEW
static void ensure_gstreamer_ready(void)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        gst_init(NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

static bool gst_element_available(const char *name)
{
    GstElementFactory *factory;
    ensure_gstreamer_ready();
    factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

static bool webrtc_preview_runtime_available(void)
{
    const char *required[] = {
        "appsrc",
        "queue",
        "audioconvert",
        "audioresample",
        "h264parse",
        "opusenc",
        "rtph264pay",
        "rtpopuspay",
        "webrtcbin",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(required); i++) {
        if (!gst_element_available(required[i])) {
            LOG_W("WebRTC preview disabled: missing GStreamer element '%s'", required[i]);
            return false;
        }
    }
    return true;
}

static void maybe_attach_colorspace_extension(GstElement *payloader)
{
    GstRTPHeaderExtension *ext;

    if (!payloader) {
        return;
    }

    ext = GST_RTP_HEADER_EXTENSION(gst_element_factory_make("rtphdrextcolorspace", NULL));
    if (!ext) {
        ext = gst_rtp_header_extension_create_from_uri(
            "http://www.webrtc.org/experiments/rtp-hdrext/color-space");
    }
    if (!ext) {
        LOG_W("WebRTC: color-space RTP header extension unavailable");
        return;
    }

    gst_rtp_header_extension_set_id(ext, 1);
    gst_rtp_header_extension_set_direction(ext, GST_RTP_HEADER_EXTENSION_DIRECTION_SENDONLY);
    gst_rtp_header_extension_set_wants_update_non_rtp_src_caps(ext, TRUE);
    g_signal_emit_by_name(payloader, "add-extension", ext);
    LOG_I("WebRTC: attached RTP color-space extension (id=1)");
    gst_object_unref(ext);
}
#endif

static void profile_free(gpointer data)
{
    sbs_preview_profile_t *profile = data;
    if (!profile) {
        return;
    }
    g_free(profile->id);
    g_free(profile->transport);
    g_free(profile->codec);
    g_free(profile->container);
    g_free(profile->latency_class);
    g_free(profile->stream_url);
    g_free(profile);
}

static const char *kind_name(sbs_preview_profile_kind_t kind)
{
    return kind == SBS_PREVIEW_PROFILE_KIND_FALLBACK ? "fallback" : "reuse";
}

static bool preview_downscale_factor_allowed(uint32_t factor)
{
    return factor == 1u || factor == 2u || factor == 4u || factor == 8u;
}

static uint32_t preview_downscale_factor_or_default(uint32_t factor)
{
    return preview_downscale_factor_allowed(factor)
        ? factor : SBS_PREVIEW_DEFAULT_DOWNSCALE_FACTOR;
}

static void compute_preview_size(uint32_t src_width,
                                 uint32_t src_height,
                                 uint32_t downscale_factor,
                                 uint32_t *out_width,
                                 uint32_t *out_height)
{
    uint32_t factor = preview_downscale_factor_or_default(downscale_factor);
    uint32_t width = src_width > 0 ? src_width : 1920u;
    uint32_t height = src_height > 0 ? src_height : 1080u;

    width /= factor;
    height /= factor;
    width &= ~1u;
    height &= ~1u;
    if (width == 0) width = 2;
    if (height == 0) height = 2;

    if (out_width) *out_width = width;
    if (out_height) *out_height = height;
}

static sbs_preview_profile_t *profile_new(const char *id,
                                          sbs_preview_profile_kind_t kind,
                                          const char *transport,
                                          const char *codec,
                                          const char *container,
                                          const char *latency_class,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t framerate,
                                          bool hardware_decode_preferred,
                                          bool requires_additional_encode,
                                          bool available,
                                          bool requestable,
                                          const char *stream_url)
{
    sbs_preview_profile_t *profile = g_new0(sbs_preview_profile_t, 1);
    profile->id = g_strdup(id);
    profile->kind = kind;
    profile->transport = g_strdup(transport);
    profile->codec = g_strdup(codec);
    profile->container = g_strdup(container);
    profile->latency_class = g_strdup(latency_class);
    profile->width = width;
    profile->height = height;
    profile->downscale_factor = 1u;
    profile->framerate = framerate;
    profile->hardware_decode_preferred = hardware_decode_preferred;
    profile->requires_additional_encode = requires_additional_encode;
    profile->available = available;
    profile->requestable = requestable;
    profile->active = available && !requires_additional_encode;
    profile->reference_color = false;
    profile->active_color_mode = SBS_PREVIEW_COLOR_MODE_SDR;
    profile->viewer_count = 0;
    profile->stream_url = g_strdup(stream_url);
    return profile;
}

static void refresh_urls(sbs_preview_engine_t *engine)
{
    sbs_preview_profile_t *profile;

    if (!engine) {
        return;
    }

    profile = g_hash_table_lookup(engine->profiles, "program-hevc-srt");
    if (profile) {
        g_free(profile->stream_url);
        profile->stream_url = g_strdup("srt://127.0.0.1:8888");
    }

    /* WebRTC profile has no URL — signaling happens over JSON-RPC */
    profile = g_hash_table_lookup(engine->profiles, "preview-h264-webrtc");
    if (profile) {
        g_free(profile->stream_url);
        profile->stream_url = g_strdup("webrtc://signaling");
    }
}

sbs_preview_engine_t *sbs_preview_engine_new(void)
{
    sbs_preview_engine_t *engine = g_new0(sbs_preview_engine_t, 1);
    bool webrtc_requestable = false;
    g_mutex_init(&engine->lock);
    engine->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, profile_free);
    engine->runtimes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, runtime_free);
    engine->api_port = 10086;

#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    webrtc_requestable = webrtc_preview_runtime_available();
#endif
    engine->webrtc_requestable = webrtc_requestable;

    g_hash_table_insert(engine->profiles, "program-hevc-srt",
        profile_new("program-hevc-srt", SBS_PREVIEW_PROFILE_KIND_REUSE,
                    "srt", "h265", "mpegts", "low",
                    3840, 2160, 60, true, false, true, false,
                    "srt://127.0.0.1:8888"));

    {
        sbs_preview_profile_t *webrtc_profile = profile_new(
            "preview-h264-webrtc", SBS_PREVIEW_PROFILE_KIND_FALLBACK,
            "webrtc", "h264", "rtp", "low",
            480, 270, 30, false, true, false, webrtc_requestable,
            "webrtc://signaling");
        webrtc_profile->downscale_factor = SBS_PREVIEW_DEFAULT_DOWNSCALE_FACTOR;
        webrtc_profile->bitrate_kbps = 2500;
        g_hash_table_insert(engine->profiles, "preview-h264-webrtc", webrtc_profile);
    }

    return engine;
}

void sbs_preview_engine_free(sbs_preview_engine_t *engine)
{
    if (!engine) {
        return;
    }
    g_hash_table_destroy(engine->runtimes);
    g_hash_table_destroy(engine->profiles);
    g_mutex_clear(&engine->lock);
    g_free(engine);
}

static sbs_preview_profile_t *preview_engine_get_profile_unlocked(const sbs_preview_engine_t *engine,
                                                                  const char *profile_id);

void sbs_preview_engine_set_source_format(sbs_preview_engine_t *engine,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t fps,
                                            sbs_preview_color_mode_t color_mode)
{
    uint32_t new_default_w = 0;
    uint32_t new_default_h = 0;
    uint32_t old_default_fps;
    uint32_t new_default_fps;

    if (!engine)
        return;
    g_mutex_lock(&engine->lock);
    old_default_fps = engine->src_fps && engine->src_fps < 30 ? engine->src_fps : 30;
    new_default_fps = fps && fps < 30 ? fps : 30;

    engine->src_width = width;
    engine->src_height = height;
    engine->src_fps = fps;
    engine->src_color_mode = color_mode;
    {
        sbs_preview_profile_t *profile = preview_engine_get_profile_unlocked(
            engine, "program-hevc-srt");
        if (profile) {
            if (width > 0) profile->width = width;
            if (height > 0) profile->height = height;
            if (fps > 0) profile->framerate = fps;
        }
    }
    {
        sbs_preview_profile_t *profile = preview_engine_get_profile_unlocked(
            engine, "preview-h264-webrtc");
        if (profile) {
            bool default_fps = profile->framerate == 0 || profile->framerate == old_default_fps;
            bool changed = false;

            profile->downscale_factor = preview_downscale_factor_or_default(profile->downscale_factor);
            compute_preview_size(width, height, profile->downscale_factor,
                                 &new_default_w, &new_default_h);
            profile->requestable = engine->webrtc_requestable;
            g_free(profile->codec);
            profile->codec = g_strdup("h264");
            changed = changed || profile->width != new_default_w || profile->height != new_default_h;
            profile->width = new_default_w;
            profile->height = new_default_h;
            if (default_fps) {
                changed = changed || profile->framerate != new_default_fps;
                profile->framerate = new_default_fps;
            }
            if (changed && profile->active && profile->requires_additional_encode) {
                g_hash_table_remove(engine->runtimes, profile->id);
                profile->active = false;
                profile->available = false;
                profile->viewer_count = 0;
            }
        }
    }
    g_mutex_unlock(&engine->lock);
}

void sbs_preview_engine_set_api_port(sbs_preview_engine_t *engine, uint16_t port)
{
    if (!engine) {
        return;
    }
    g_mutex_lock(&engine->lock);
    engine->api_port = port;
    refresh_urls(engine);
    g_mutex_unlock(&engine->lock);
}

void sbs_preview_engine_set_ice_callback(sbs_preview_engine_t *engine,
                                         sbs_preview_ice_callback_t callback,
                                         void *userdata)
{
    if (!engine) return;
    g_mutex_lock(&engine->lock);
    engine->ice_callback = callback;
    engine->ice_callback_userdata = userdata;
    g_mutex_unlock(&engine->lock);
}

static sbs_preview_profile_t *preview_engine_get_profile_unlocked(const sbs_preview_engine_t *engine,
                                                                  const char *profile_id)
{
    if (!engine || !profile_id) {
        return NULL;
    }
    return g_hash_table_lookup(engine->profiles, profile_id);
}

static sbs_preview_profile_t *preview_engine_active_fallback_unlocked(const sbs_preview_engine_t *engine)
{
    GHashTableIter iter;
    gpointer key, value;

    if (!engine) {
        return NULL;
    }
    g_hash_table_iter_init(&iter, engine->profiles);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_preview_profile_t *profile = value;
        if (profile->kind == SBS_PREVIEW_PROFILE_KIND_FALLBACK && profile->active) {
            return profile;
        }
    }
    return NULL;
}

sbs_preview_profile_t *sbs_preview_engine_get_profile(const sbs_preview_engine_t *engine,
                                                      const char *profile_id)
{
    sbs_preview_profile_t *profile;

    if (!engine || !profile_id)
        return NULL;
    g_mutex_lock((GMutex *)&engine->lock);
    profile = preview_engine_get_profile_unlocked(engine, profile_id);
    g_mutex_unlock((GMutex *)&engine->lock);
    return profile;
}

sbs_preview_profile_t *sbs_preview_engine_active_fallback(const sbs_preview_engine_t *engine)
{
    sbs_preview_profile_t *profile;

    if (!engine)
        return NULL;
    g_mutex_lock((GMutex *)&engine->lock);
    profile = preview_engine_active_fallback_unlocked(engine);
    g_mutex_unlock((GMutex *)&engine->lock);
    return profile;
}

#ifdef SBS_HAVE_GSTREAMER_PREVIEW

static void set_appsrc_queue_limits(GstElement *appsrc,
                                    guint64 max_bytes,
                                    guint max_buffers)
{
    if (!appsrc)
        return;

    g_object_set(appsrc,
        "block", FALSE,
        "max-bytes", max_bytes,
        NULL);

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(appsrc), "max-buffers")) {
        g_object_set(appsrc, "max-buffers", max_buffers, NULL);
    }
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(appsrc), "leaky-type")) {
        g_object_set(appsrc, "leaky-type", 2, NULL); /* downstream: drop oldest */
    }
}

/* ── WebRTC callbacks ─────────────────────────────────────────── */

static void on_ice_candidate(GstElement *webrtcbin, guint mline_index,
                             gchar *candidate, gpointer user_data)
{
    preview_runtime_t *runtime = user_data;
    (void)webrtcbin;

    LOG_I("WebRTC ICE candidate: mline=%u %s", mline_index, candidate);

    if (runtime->engine && runtime->engine->ice_callback) {
        runtime->engine->ice_callback(runtime->engine->ice_callback_userdata,
                                      mline_index, candidate);
    }

    /* Also buffer for late retrieval */
    g_mutex_lock(&runtime->ice_mutex);
    if (runtime->ice_candidates) {
        char *entry = g_strdup_printf("%u:%s", mline_index, candidate);
        g_ptr_array_add(runtime->ice_candidates, entry);
    }
    g_mutex_unlock(&runtime->ice_mutex);
}

static void on_offer_created(GstPromise *promise, gpointer user_data)
{
    preview_runtime_t *runtime = user_data;
    const GstStructure *reply;
    GstWebRTCSessionDescription *offer = NULL;
    GstWebRTCSessionDescription *local_offer = NULL;
    GstSDPMessage *local_sdp = NULL;
    gchar *sdp_text;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    if (!offer) {
        LOG_E("WebRTC: failed to get offer from promise");
        gst_promise_unref(promise);
        return;
    }

    sdp_text = gst_sdp_message_as_text(offer->sdp);
    LOG_I("WebRTC SDP offer created (%zu bytes)", strlen(sdp_text));

    /* For true HDR10 preview, advertise the High 10 profile honestly and let
     * the browser reject it if unsupported.  The WebUI can then retry the
     * SDR-reference path. */
    if (runtime->active_color_mode != SBS_PREVIEW_COLOR_MODE_HDR10) {
        char *p = strstr(sdp_text, "profile-level-id=");
        if (p) {
            p += strlen("profile-level-id=");
            /* Overwrite the 6-char hex value in-place */
            if (strlen(p) >= 6) {
                memcpy(p, "42e01f", 6);
            }
        }
        /* Remove sprop-parameter-sets from fmtp line */
        p = strstr(sdp_text, "sprop-parameter-sets=");
        if (p) {
            /* Find the start of this parameter (after ; or space) */
            char *param_start = p;
            /* Find the end (next ; or end of value) */
            char *param_end = p;
            while (*param_end && *param_end != ';' && *param_end != '\r' && *param_end != '\n') {
                param_end++;
            }
            /* If followed by ;, skip the semicolon too */
            if (*param_end == ';') {
                param_end++;
            }
            /* Remove by shifting the rest of the string */
            memmove(param_start, param_end, strlen(param_end) + 1);
        }
    }

    if (gst_sdp_message_new(&local_sdp) == GST_SDP_OK &&
        gst_sdp_message_parse_buffer((const guint8 *)sdp_text,
                                     strlen(sdp_text), local_sdp) == GST_SDP_OK) {
        local_offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER,
                                                         local_sdp);
    } else {
        if (local_sdp) {
            gst_sdp_message_free(local_sdp);
        }
        local_offer = gst_webrtc_session_description_copy(offer);
        LOG_W("WebRTC: using original local offer after SDP parse failure");
    }

    g_signal_emit_by_name(runtime->webrtcbin, "set-local-description", local_offer, NULL);

    g_free(runtime->local_sdp);
    runtime->local_sdp = sdp_text;

    g_mutex_lock(&runtime->ready_mutex);
    runtime->webrtc_ready = true;
    g_cond_signal(&runtime->ready_cond);
    g_mutex_unlock(&runtime->ready_mutex);

    gst_webrtc_session_description_free(local_offer);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

static void on_negotiation_needed(GstElement *webrtcbin, gpointer user_data)
{
    preview_runtime_t *runtime = user_data;
    GstPromise *promise;
    (void)webrtcbin;

    /* Only create the offer once; subsequent negotiation-needed signals
     * (e.g. after setting the remote answer) must be ignored to avoid
     * disrupting the already-established session. */
    if (runtime->webrtc_ready) {
        LOG_D("WebRTC: ignoring negotiation-needed (offer already created)");
        return;
    }

    LOG_I("WebRTC: negotiation needed, creating offer");
    promise = gst_promise_new_with_change_func(on_offer_created, runtime, NULL);
    g_signal_emit_by_name(runtime->webrtcbin, "create-offer", NULL, promise);
}

static gboolean on_pipeline_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void)bus;
    (void)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOG_E("WebRTC pipeline error from %s: %s (%s)",
              GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        LOG_W("WebRTC pipeline warning from %s: %s (%s)",
              GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_IS_PIPELINE(msg->src)) {
            GstState old_state, new_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
            LOG_D("WebRTC pipeline state: %s -> %s",
                  gst_element_state_get_name(old_state),
                  gst_element_state_get_name(new_state));
        }
        break;
    case GST_MESSAGE_EOS:
        LOG_W("WebRTC pipeline EOS");
        break;
    default:
        break;
    }
    return TRUE;
}

static void on_webrtc_state_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    gint state = 0;
    (void)user_data;

    g_object_get(object, pspec->name, &state, NULL);
    LOG_I("WebRTC state %s=%d", pspec->name, state);
}

typedef struct webrtc_answer_task {
    GstElement *webrtcbin;
    char *sdp_answer;
    int rc;
    GMutex mutex;
    GCond cond;
    bool done;
} webrtc_answer_task_t;

static gboolean set_webrtc_answer_on_context(gpointer user_data)
{
    webrtc_answer_task_t *task = user_data;
    GstSDPMessage *sdp_msg = NULL;
    GstWebRTCSessionDescription *answer;

    task->rc = SBS_OK;

    if (gst_sdp_message_new(&sdp_msg) != GST_SDP_OK) {
        task->rc = SBS_ERR_NOMEM;
        goto done;
    }

    if (gst_sdp_message_parse_buffer((const guint8 *)task->sdp_answer,
                                     strlen(task->sdp_answer), sdp_msg) != GST_SDP_OK) {
        gst_sdp_message_free(sdp_msg);
        LOG_E("WebRTC: failed to parse SDP answer");
        task->rc = SBS_ERR_INVAL;
        goto done;
    }

    answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
    g_signal_emit_by_name(task->webrtcbin, "set-remote-description", answer, NULL);
    gst_webrtc_session_description_free(answer);
    LOG_I("WebRTC: remote SDP answer set");

done:
    g_mutex_lock(&task->mutex);
    task->done = true;
    g_cond_signal(&task->cond);
    g_mutex_unlock(&task->mutex);
    return G_SOURCE_REMOVE;
}

typedef struct webrtc_ice_task {
    GstElement *webrtcbin;
    unsigned int mline_index;
    char *candidate;
} webrtc_ice_task_t;

static gboolean add_webrtc_ice_on_context(gpointer user_data)
{
    webrtc_ice_task_t *task = user_data;

    g_signal_emit_by_name(task->webrtcbin, "add-ice-candidate",
                          task->mline_index, task->candidate);
    LOG_D("WebRTC: added remote ICE candidate mline=%u", task->mline_index);

    gst_object_unref(task->webrtcbin);
    g_free(task->candidate);
    g_free(task);
    return G_SOURCE_REMOVE;
}

/* Thread function for the dedicated WebRTC main loop */
static gpointer preview_thread_func(gpointer data)
{
    preview_runtime_t *runtime = data;
    g_main_context_push_thread_default(runtime->context);
    LOG_I("WebRTC preview thread started");
    g_main_loop_run(runtime->loop);
    LOG_I("WebRTC preview thread exiting");
    g_main_context_pop_thread_default(runtime->context);
    return NULL;
}

static preview_runtime_t *start_webrtc_runtime(sbs_preview_engine_t *engine,
                                               sbs_preview_profile_t *profile)
{
    preview_runtime_t *runtime = NULL;
    GstElement *pipeline = NULL, *appsrc = NULL;
    GstElement *parser = NULL, *payloader = NULL, *webrtcbin = NULL;
    GstElement *audio_appsrc = NULL, *audio_queue = NULL;
    GstElement *aconv = NULL, *aresample = NULL, *opusenc = NULL, *opuspay = NULL;
    GstCaps *src_caps = NULL;
    GstCaps *audio_caps = NULL;
    gboolean hdr10;
    bool context_pushed = false;
    const char *codec;
    const char *caps_name;
    const char *parser_name;
    const char *payloader_name;

    uint32_t src_w  = profile->width ? profile->width : (engine->src_width ? engine->src_width : 1920);
    uint32_t src_h  = profile->height ? profile->height : (engine->src_height ? engine->src_height : 1080);
    uint32_t src_fps = profile->framerate ? profile->framerate : (engine->src_fps ? engine->src_fps : 60);
    hdr10 = profile->active_color_mode == SBS_PREVIEW_COLOR_MODE_HDR10;
    codec = "h264";
    caps_name = "video/x-h264";
    parser_name = "h264parse";
    payloader_name = "rtph264pay";

    ensure_gstreamer_ready();

    runtime = g_new0(preview_runtime_t, 1);
    g_mutex_init(&runtime->ice_mutex);
    g_mutex_init(&runtime->ready_mutex);
    g_cond_init(&runtime->ready_cond);
    runtime->ice_candidates = g_ptr_array_new_with_free_func(g_free);
    runtime->engine = engine;
    runtime->reference_color = profile->reference_color;
    runtime->active_color_mode = profile->active_color_mode;

    /* Create a dedicated GMainContext for this WebRTC pipeline so that
     * webrtcbin's internal DTLS/RTCP/ICE processing isn't starved by the
     * compositor's main loop. */
    runtime->context = g_main_context_new();
    runtime->loop = g_main_loop_new(runtime->context, FALSE);

    /* Push as thread-default so GStreamer elements (especially webrtcbin)
     * attach their sources to this context instead of the default one. */
    g_main_context_push_thread_default(runtime->context);
    context_pushed = true;

    /* Build pipeline manually for better control */
    pipeline = gst_pipeline_new("preview-webrtc");

    appsrc = gst_element_factory_make("appsrc", "src");
    if (appsrc) {
        g_object_set(appsrc,
            "is-live", TRUE,
            "do-timestamp", FALSE,
            "format", GST_FORMAT_TIME,
            NULL);
        set_appsrc_queue_limits(appsrc, 2u * 1024u * 1024u, 4u);
        src_caps = gst_caps_new_simple(caps_name,
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            NULL);
        g_object_set(appsrc, "caps", src_caps, NULL);
        gst_caps_unref(src_caps);
    }

    uint32_t bitrate = profile->bitrate_kbps ? profile->bitrate_kbps : 2500;
    {
        sbs_direct_venc_config_t venc_cfg = {
            .codec = codec,
            .width = src_w,
            .height = src_h,
            .fps_num = src_fps,
            .fps_den = 1,
            .bitrate_kbps = bitrate,
            .gop_size = src_fps,
            .gop_pattern = 0,
            .rc_mode = 0,
            .hdr10 = hdr10,
        };
        runtime->direct_enc = sbs_direct_venc_new(&venc_cfg);
    }

    parser = gst_element_factory_make(parser_name, NULL);
    if (parser) {
        g_object_set(parser,
            "config-interval", -1,
            "disable-passthrough", TRUE,
            NULL);
    }

    payloader = gst_element_factory_make(payloader_name, NULL);
    if (payloader) {
        g_object_set(payloader,
            "config-interval", -1,
            "pt", 96,
            NULL);
        g_object_set(payloader,
            "aggregate-mode", 1, /* zero-latency: aggregate NALs into fewer packets */
            NULL);
    }
    maybe_attach_colorspace_extension(payloader);

    audio_appsrc = gst_element_factory_make("appsrc", "audio_src");
    if (audio_appsrc) {
        g_object_set(audio_appsrc,
            "is-live", TRUE,
            "do-timestamp", FALSE,
            "format", GST_FORMAT_TIME,
            "block", FALSE,
            "max-bytes", 48000 * 2 * 2,
            NULL);
        audio_caps = gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, "S16LE",
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "layout", G_TYPE_STRING, "interleaved",
            NULL);
        g_object_set(audio_appsrc, "caps", audio_caps, NULL);
        gst_caps_unref(audio_caps);
    }
    audio_queue = gst_element_factory_make("queue", "audio_q");
    if (audio_queue) {
        g_object_set(audio_queue,
            "max-size-buffers", 0,
            "max-size-bytes", 0,
            "max-size-time", (guint64)(2 * GST_SECOND),
            "leaky", 2,
            NULL);
    }
    aconv = gst_element_factory_make("audioconvert", "audio_convert");
    aresample = gst_element_factory_make("audioresample", "audio_resample");
    opusenc = gst_element_factory_make("opusenc", "opus_enc");
    if (opusenc) {
        g_object_set(opusenc,
            "bitrate", 128000,
            NULL);
    }
    opuspay = gst_element_factory_make("rtpopuspay", "opus_pay");
    if (opuspay) {
        g_object_set(opuspay,
            "pt", 97,
            NULL);
    }

    webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    if (webrtcbin) {
        g_object_set(webrtcbin,
            "bundle-policy", 3, /* max-bundle */
            "message-forward", TRUE,
            NULL);
    }

    if (!pipeline || !appsrc || !runtime->direct_enc ||
        !parser || !payloader || !audio_appsrc || !audio_queue ||
        !aconv || !aresample || !opusenc || !opuspay || !webrtcbin) {
        LOG_E("WebRTC: failed to create one or more pipeline elements");
        if (!appsrc) LOG_E("WebRTC: missing appsrc element");
        if (!runtime->direct_enc) LOG_E("WebRTC: failed to create direct %s encoder", codec);
        if (!parser) LOG_E("WebRTC: missing %s element", parser_name);
        if (!payloader) LOG_E("WebRTC: missing %s element", payloader_name);
        if (!audio_appsrc) LOG_E("WebRTC: missing audio appsrc element");
        if (!audio_queue) LOG_E("WebRTC: missing audio queue element");
        if (!aconv) LOG_E("WebRTC: missing audioconvert element");
        if (!aresample) LOG_E("WebRTC: missing audioresample element");
        if (!opusenc) LOG_E("WebRTC: missing opusenc element");
        if (!opuspay) LOG_E("WebRTC: missing rtpopuspay element");
        if (!webrtcbin) LOG_E("WebRTC: missing webrtcbin element");
        if (context_pushed) {
            g_main_context_pop_thread_default(runtime->context);
            context_pushed = false;
        }
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (parser) gst_object_unref(parser);
        if (payloader) gst_object_unref(payloader);
        if (audio_appsrc) gst_object_unref(audio_appsrc);
        if (audio_queue) gst_object_unref(audio_queue);
        if (aconv) gst_object_unref(aconv);
        if (aresample) gst_object_unref(aresample);
        if (opusenc) gst_object_unref(opusenc);
        if (opuspay) gst_object_unref(opuspay);
        if (webrtcbin) gst_object_unref(webrtcbin);
        runtime_free(runtime);
        return NULL;
    }

    gst_bin_add_many(GST_BIN(pipeline),
        appsrc, parser, payloader,
        audio_appsrc, audio_queue, aconv, aresample, opusenc, opuspay,
        webrtcbin, NULL);

    GstElement *queue1 = gst_element_factory_make("queue", "q1");
    if (!queue1) {
        LOG_E("WebRTC: missing queue element");
        if (context_pushed) {
            g_main_context_pop_thread_default(runtime->context);
            context_pushed = false;
        }
        gst_object_unref(pipeline);
        runtime_free(runtime);
        return NULL;
    }
    g_object_set(queue1, "leaky", 2 /* downstream */, "max-size-buffers", 5, NULL);

    gst_bin_add(GST_BIN(pipeline), queue1);

    if (!gst_element_link_many(appsrc, queue1,
                                parser, payloader, NULL)) {
        LOG_E("WebRTC: failed to link encode chain");
        if (context_pushed) {
            g_main_context_pop_thread_default(runtime->context);
            context_pushed = false;
        }
        gst_object_unref(pipeline);
        runtime_free(runtime);
        return NULL;
    }

    if (!gst_element_link_many(audio_appsrc, audio_queue,
                               aconv, aresample, opusenc, opuspay, NULL)) {
        LOG_E("WebRTC: failed to link audio encode chain");
        if (context_pushed) {
            g_main_context_pop_thread_default(runtime->context);
            context_pushed = false;
        }
        gst_object_unref(pipeline);
        runtime_free(runtime);
        return NULL;
    }

    /* Link payloader to webrtcbin via request pad */
    {
        GstPad *pay_src = gst_element_get_static_pad(payloader, "src");
        GstPad *webrtc_sink = gst_element_request_pad_simple(webrtcbin, "sink_%u");
        if (!pay_src || !webrtc_sink ||
            gst_pad_link(pay_src, webrtc_sink) != GST_PAD_LINK_OK) {
            LOG_E("WebRTC: failed to link payloader to webrtcbin");
            if (pay_src) gst_object_unref(pay_src);
            if (webrtc_sink) gst_object_unref(webrtc_sink);
            if (context_pushed) {
                g_main_context_pop_thread_default(runtime->context);
                context_pushed = false;
            }
            gst_object_unref(pipeline);
            runtime_free(runtime);
            return NULL;
        }
        gst_object_unref(pay_src);
        gst_object_unref(webrtc_sink);
    }

    /* Link Opus payloader to webrtcbin via a second request pad. */
    {
        GstPad *pay_src = gst_element_get_static_pad(opuspay, "src");
        GstPad *webrtc_sink = gst_element_request_pad_simple(webrtcbin, "sink_%u");
        if (!pay_src || !webrtc_sink ||
            gst_pad_link(pay_src, webrtc_sink) != GST_PAD_LINK_OK) {
            LOG_E("WebRTC: failed to link audio payloader to webrtcbin");
            if (pay_src) gst_object_unref(pay_src);
            if (webrtc_sink) gst_object_unref(webrtc_sink);
            if (context_pushed) {
                g_main_context_pop_thread_default(runtime->context);
                context_pushed = false;
            }
            gst_object_unref(pipeline);
            runtime_free(runtime);
            return NULL;
        }
        gst_object_unref(pay_src);
        gst_object_unref(webrtc_sink);
    }

    /* Connect WebRTC signals */
    g_signal_connect(webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), runtime);
    g_signal_connect(webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), runtime);
    g_signal_connect(webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(on_webrtc_state_notify), runtime);
    g_signal_connect(webrtcbin, "notify::connection-state",
                     G_CALLBACK(on_webrtc_state_notify), runtime);
    g_signal_connect(webrtcbin, "notify::signaling-state",
                     G_CALLBACK(on_webrtc_state_notify), runtime);

    runtime->pipeline = pipeline;
    runtime->appsrc = gst_object_ref(appsrc);
    runtime->audio_appsrc = gst_object_ref(audio_appsrc);
    runtime->webrtcbin = gst_object_ref(webrtcbin);

    /* With do-timestamp=TRUE on appsrc, the pipeline clock generates RTP
     * timestamps that match actual frame arrival times, which keeps the
     * browser's jitter buffer happy.  Use system clock. */
    /* Re-disabled: clock causes wildly incorrect PTS.  Use manual PTS instead. */
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), NULL);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOG_E("WebRTC: pipeline failed to enter PLAYING");
        if (context_pushed) {
            g_main_context_pop_thread_default(runtime->context);
            context_pushed = false;
        }
        runtime_free(runtime);
        return NULL;
    }

    /* Attach a bus watch to the preview context (not the default one) */
    {
        GstBus *bus = gst_element_get_bus(pipeline);
        GSource *bus_source = gst_bus_create_watch(bus);
        g_source_set_callback(bus_source, (GSourceFunc)on_pipeline_bus_message, runtime, NULL);
        g_source_attach(bus_source, runtime->context);
        g_source_unref(bus_source);
        gst_object_unref(bus);
    }

    /* Pop thread-default before spawning the thread (it will push its own) */
    g_main_context_pop_thread_default(runtime->context);
    context_pushed = false;

    /* Start the dedicated thread which runs the GMainLoop for webrtcbin */
    runtime->thread = g_thread_new("webrtc-preview", preview_thread_func, runtime);

    LOG_I("WebRTC preview runtime active for %s (src=%ux%u@%u %s enc=%ux%u@%u %s)",
          profile->id, src_w, src_h, src_fps,
          hdr10 ? "bt2100-pq" : "bt709",
          profile->width, profile->height, profile->framerate,
          hdr10 ? "direct-h264-hdr10" : "direct-h264");
    return runtime;
}

/* read_frame_to_buffer — REMOVED (was memfd mmap+memcpy path) */
#endif

int sbs_preview_engine_ensure_profile(sbs_preview_engine_t *engine,
                                       const char *profile_id,
                                       sbs_preview_profile_t **out_profile)
{
    sbs_preview_profile_t *profile;
    preview_runtime_t *runtime;

    if (!engine)
        return SBS_ERR_INVAL;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_get_profile_unlocked(engine, profile_id);
    if (!profile) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_NOT_FOUND;
    }

    if (!profile->available && !profile->requestable) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_INVAL;
    }

    runtime = g_hash_table_lookup(engine->runtimes, profile->id);
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    if (profile->requires_additional_encode) {
        /* Tear down any existing runtime so we get a fresh WebRTC session
         * (new DTLS fingerprint, new ICE credentials, new SDP offer).
         * Each browser "Start Preview" click needs a clean session. */
        if (runtime) {
            LOG_I("tearing down existing WebRTC runtime for %s (new session requested)",
                  profile->id);
            g_hash_table_remove(engine->runtimes, profile->id);
            runtime = NULL;
            /* Brief delay to let amlvenc fully release HW encoder resources
             * before creating a new instance. */
            g_usleep(100000); /* 100ms */
        }
        runtime = start_webrtc_runtime(engine, profile);
        if (!runtime) {
            LOG_E("unable to start WebRTC preview runtime for %s", profile->id);
            g_mutex_unlock(&engine->lock);
            return SBS_ERR_IO;
        }
        g_hash_table_insert(engine->runtimes, profile->id, runtime);
    }
#endif

    profile->available = true;
    profile->active = true;
    profile->viewer_count = 1;
    if (out_profile) {
        *out_profile = profile;
    }
    g_mutex_unlock(&engine->lock);
    return SBS_OK;
}

int sbs_preview_engine_release_profile(sbs_preview_engine_t *engine,
                                       const char *profile_id,
                                       sbs_preview_profile_t **out_profile)
{
    sbs_preview_profile_t *profile;
    if (!engine)
        return SBS_ERR_INVAL;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_get_profile_unlocked(engine, profile_id);
    if (!profile) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_NOT_FOUND;
    }

    if (profile->viewer_count > 0) {
        profile->viewer_count--;
    }
    if (profile->requires_additional_encode && profile->viewer_count == 0) {
        LOG_I("stopping preview runtime for %s", profile->id);
        g_hash_table_remove(engine->runtimes, profile->id);
        profile->active = false;
        profile->available = false;
        profile->reference_color = false;
        profile->active_color_mode = SBS_PREVIEW_COLOR_MODE_SDR;
    }
    if (out_profile) {
        *out_profile = profile;
    }
    g_mutex_unlock(&engine->lock);
    return SBS_OK;
}

void sbs_preview_engine_update_metrics(sbs_preview_engine_t *engine,
                                       uint64_t produced,
                                       uint64_t dropped,
                                       double latency_ms,
                                       bool degraded)
{
    if (!engine) {
        return;
    }
    engine->total_frames_produced = produced;
    engine->total_frames_dropped = dropped;
    engine->latency_ms = latency_ms;
    engine->degraded = degraded;
}

#ifdef SBS_HAVE_GSTREAMER_PREVIEW
static uint8_t *preview_copy_nv21_tight(const sbs_video_frame_msg_t *msg,
                                        const uint8_t *data,
                                        size_t size,
                                        sbs_video_frame_msg_t *tight_msg,
                                        size_t *tight_size)
{
    uint32_t width, height, y_stride, uv_stride, uv_offset;
    size_t required_size, out_size;
    uint8_t *out;

    if (!msg || !data || !tight_msg || !tight_size || msg->n_planes < 2)
        return NULL;
    if (msg->drm_format != 0 && msg->drm_format != DRM_FORMAT_NV21)
        return NULL;

    width = msg->width;
    height = msg->height;
    if (width == 0 || height == 0 || (width & 1u) || (height & 1u))
        return NULL;

    y_stride = msg->plane_stride[0] ? msg->plane_stride[0] : width;
    uv_stride = msg->plane_stride[1] ? msg->plane_stride[1] : width;
    uv_offset = msg->plane_offset[1] ? msg->plane_offset[1] : y_stride * height;

    if (msg->plane_offset[0] == 0 && y_stride == width &&
        uv_offset == width * height && uv_stride == width) {
        return NULL;
    }

    required_size = (size_t)uv_offset + (size_t)uv_stride * (height / 2u);
    if (size < required_size) {
        LOG_W("preview NV21 tight copy skipped: buffer too small size=%zu required=%zu stride=%u/%u offset=%u %ux%u",
              size, required_size, y_stride, uv_stride, uv_offset, width, height);
        return NULL;
    }

    out_size = (size_t)width * height * 3u / 2u;
    out = g_malloc(out_size);
    for (uint32_t y = 0; y < height; y++) {
        memcpy(out + (size_t)y * width,
               data + msg->plane_offset[0] + (size_t)y * y_stride,
               width);
    }
    for (uint32_t y = 0; y < height / 2u; y++) {
        memcpy(out + (size_t)width * height + (size_t)y * width,
               data + uv_offset + (size_t)y * uv_stride,
               width);
    }

    *tight_msg = *msg;
    tight_msg->plane_offset[0] = 0;
    tight_msg->plane_offset[1] = width * height;
    tight_msg->plane_stride[0] = width;
    tight_msg->plane_stride[1] = width;
    *tight_size = out_size;
    return out;
}
#endif

/* sbs_preview_engine_consume_frame — REMOVED (was fd-based memfd path) */

void sbs_preview_engine_consume_frame_ptr(sbs_preview_engine_t *engine,
                                             const sbs_video_frame_msg_t *msg,
                                             const void *data,
                                            size_t size)
{
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    sbs_preview_profile_t *profile;
    preview_runtime_t *runtime;

    if (!engine || !msg || !data || size == 0)
        return;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_active_fallback_unlocked(engine);
    if (!profile) {
        goto done;
    }

    runtime = g_hash_table_lookup(engine->runtimes, profile->id);
    if (!runtime || !runtime->appsrc) {
        goto done;
    }

    if (runtime->direct_enc) {
        sbs_direct_venc_packet_t packet;
        sbs_video_frame_msg_t tight_msg;
        const sbs_video_frame_msg_t *submit_msg = msg;
        const void *submit_data = data;
        size_t submit_size = size;
        uint8_t *tight_data = preview_copy_nv21_tight(
            msg, data, size, &tight_msg, &submit_size);
        bool force_idr = runtime->frames_pushed == 0;

        if (tight_data) {
            submit_msg = &tight_msg;
            submit_data = tight_data;
        }

        int rc = sbs_direct_venc_submit_ptr(runtime->direct_enc, submit_msg,
                                            submit_data, submit_size,
                                            force_idr, &packet);
        g_free(tight_data);
        if (rc != SBS_OK) {
            engine->total_frames_dropped++;
            goto done;
        }
        if (!packet.data || packet.size == 0) {
            goto done;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
        if (!buffer) {
            engine->total_frames_dropped++;
            goto done;
        }
        if (gst_buffer_fill(buffer, 0, packet.data, packet.size) != packet.size) {
            gst_buffer_unref(buffer);
            engine->total_frames_dropped++;
            goto done;
        }

        {
            uint32_t fps = profile->framerate ? profile->framerate : 30;
            GstClockTime frame_dur = GST_SECOND / fps;
            GST_BUFFER_PTS(buffer) = runtime->frames_pushed * frame_dur;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = frame_dur;
            if (packet.is_keyframe) {
                GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            } else {
                GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            }
        }

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(runtime->appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            engine->total_frames_dropped++;
            goto done;
        }

        runtime->frames_pushed++;
        if (runtime->frames_pushed <= 3 || packet.is_keyframe ||
            runtime->frames_pushed % 300 == 0) {
            LOG_I("preview encoded frame pushed #%lu (%zu bytes key=%d)",
                  (unsigned long)runtime->frames_pushed,
                  packet.size, packet.is_keyframe ? 1 : 0);
        }
        goto done;
    }

    /* Copy into GStreamer-owned system memory. The export slot is released
     * immediately after this call, while amlvenc consumes buffers async. */
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    if (!buffer) {
        engine->total_frames_dropped++;
        goto done;
    }
    if (gst_buffer_fill(buffer, 0, data, size) != size) {
        gst_buffer_unref(buffer);
        engine->total_frames_dropped++;
        goto done;
    }

    if (msg->n_planes >= 2) {
        gsize offsets[GST_VIDEO_MAX_PLANES] = { msg->plane_offset[0], msg->plane_offset[1], 0, 0 };
        gint strides[GST_VIDEO_MAX_PLANES] = { (gint)msg->plane_stride[0], (gint)msg->plane_stride[1], 0, 0 };
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                       GST_VIDEO_FORMAT_NV21,
                                       msg->width, msg->height,
                                       2, offsets, strides);
    }

    {
        uint32_t fps = profile->framerate ? profile->framerate : 30;
        GstClockTime frame_dur = GST_SECOND / fps;
        GST_BUFFER_PTS(buffer) = runtime->frames_pushed * frame_dur;
        GST_BUFFER_DURATION(buffer) = frame_dur;
    }

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(runtime->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        engine->total_frames_dropped++;
        goto done;
    }
    runtime->frames_pushed++;
    if (runtime->frames_pushed <= 3 || runtime->frames_pushed % 300 == 0) {
        LOG_I("preview frame pushed (ptr) #%lu (%ux%u)",
              (unsigned long)runtime->frames_pushed,
              msg->width, msg->height);
    }
done:
    g_mutex_unlock(&engine->lock);
#else
    (void)engine; (void)msg; (void)data; (void)size;
#endif
}

void sbs_preview_engine_consume_audio(sbs_preview_engine_t *engine,
                                      const sbs_audio_buffer_msg_t *msg,
                                      const void *data)
{
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    sbs_preview_profile_t *profile;
    preview_runtime_t *runtime;
    GstBuffer *buffer;
    GstFlowReturn ret;
    uint64_t pts_ns;
    uint64_t duration_ns;

    if (!engine || !msg || !data || msg->data_size == 0)
        return;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_active_fallback_unlocked(engine);
    if (!profile) {
        goto done;
    }

    runtime = g_hash_table_lookup(engine->runtimes, profile->id);
    if (!runtime || !runtime->audio_appsrc) {
        goto done;
    }

    buffer = gst_buffer_new_allocate(NULL, msg->data_size, NULL);
    if (!buffer) {
        goto done;
    }
    if (gst_buffer_fill(buffer, 0, data, msg->data_size) != msg->data_size) {
        gst_buffer_unref(buffer);
        goto done;
    }

    duration_ns = msg->duration_ns != UINT64_MAX
        ? msg->duration_ns
        : (msg->sample_rate > 0
            ? ((uint64_t)msg->n_samples * GST_SECOND) / msg->sample_rate
            : 10 * GST_MSECOND);

    if (msg->pts_ns == UINT64_MAX) {
        pts_ns = runtime->audio_buffers_pushed * duration_ns;
    } else {
        if (!runtime->audio_pts_origin_valid) {
            runtime->audio_pts_origin_ns = msg->pts_ns;
            runtime->audio_pts_origin_valid = true;
            LOG_I("preview audio clock origin: pts=%luns",
                  (unsigned long)runtime->audio_pts_origin_ns);
        }
        pts_ns = msg->pts_ns >= runtime->audio_pts_origin_ns
            ? msg->pts_ns - runtime->audio_pts_origin_ns
            : 0;
    }

    GST_BUFFER_PTS(buffer) = pts_ns;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = duration_ns;

    ret = gst_app_src_push_buffer(GST_APP_SRC(runtime->audio_appsrc), buffer);
    if (ret == GST_FLOW_OK) {
        runtime->audio_buffers_pushed++;
        if (runtime->audio_buffers_pushed <= 3 || runtime->audio_buffers_pushed % 600 == 0) {
            LOG_I("preview audio pushed #%lu samples=%u size=%u",
                  (unsigned long)runtime->audio_buffers_pushed,
                  msg->n_samples,
                  msg->data_size);
        }
    } else if (runtime->audio_buffers_pushed % 100 == 0) {
        LOG_W("preview audio appsrc push failed: %s", gst_flow_get_name(ret));
    }

done:
    g_mutex_unlock(&engine->lock);
#else
    (void)engine; (void)msg; (void)data;
#endif
}

void sbs_preview_engine_consume_frame_dmabuf(sbs_preview_engine_t *engine,
                                               const sbs_video_frame_msg_t *msg,
                                              int dmabuf_fd,
                                              size_t size)
{
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    sbs_preview_profile_t *profile;
    preview_runtime_t *runtime;
    bool close_fd = true;

    if (!engine || !msg || dmabuf_fd < 0 || size == 0)
        return;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_active_fallback_unlocked(engine);
    if (!profile) {
        goto done;
    }

    runtime = g_hash_table_lookup(engine->runtimes, profile->id);
    if (!runtime || !runtime->appsrc) {
        goto done;
    }

    if (runtime->direct_enc) {
        sbs_direct_venc_packet_t packet;
        bool force_idr = runtime->frames_pushed == 0;
        int rc = sbs_direct_venc_submit_dmabuf(runtime->direct_enc, msg,
                                               dmabuf_fd, size, force_idr,
                                               &packet);
        if (rc != SBS_OK) {
            engine->total_frames_dropped++;
            goto done;
        }
        if (!packet.data || packet.size == 0) {
            goto done;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
        if (!buffer) {
            engine->total_frames_dropped++;
            goto done;
        }
        if (gst_buffer_fill(buffer, 0, packet.data, packet.size) != packet.size) {
            gst_buffer_unref(buffer);
            engine->total_frames_dropped++;
            goto done;
        }

        {
            uint32_t fps = profile->framerate ? profile->framerate : 30;
            GstClockTime frame_dur = GST_SECOND / fps;
            GST_BUFFER_PTS(buffer) = runtime->frames_pushed * frame_dur;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = frame_dur;
            if (packet.is_keyframe) {
                GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            } else {
                GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            }
        }

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(runtime->appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            engine->total_frames_dropped++;
            goto done;
        }

        runtime->frames_pushed++;
        if (runtime->frames_pushed <= 3 || packet.is_keyframe ||
            runtime->frames_pushed % 300 == 0) {
            LOG_I("preview encoded dmabuf frame pushed #%lu (%zu bytes key=%d)",
                  (unsigned long)runtime->frames_pushed,
                  packet.size, packet.is_keyframe ? 1 : 0);
        }
        goto done;
    }

    GstAllocator *alloc = gst_dmabuf_allocator_new();
    if (!alloc) {
        engine->total_frames_dropped++;
        goto done;
    }
    GstMemory *mem = gst_dmabuf_allocator_alloc(alloc, dmabuf_fd, size);
    gst_object_unref(alloc);
    if (!mem) {
        engine->total_frames_dropped++;
        goto done;
    }
    close_fd = false;

    GstBuffer *buffer = gst_buffer_new();
    if (!buffer) {
        gst_memory_unref(mem);
        engine->total_frames_dropped++;
        goto done;
    }
    gst_buffer_append_memory(buffer, mem);

    if (msg->n_planes >= 2) {
        gsize offsets[GST_VIDEO_MAX_PLANES] = { msg->plane_offset[0], msg->plane_offset[1], 0, 0 };
        gint strides[GST_VIDEO_MAX_PLANES] = { (gint)msg->plane_stride[0], (gint)msg->plane_stride[1], 0, 0 };
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                       GST_VIDEO_FORMAT_NV21,
                                       msg->width, msg->height,
                                       2, offsets, strides);
    }

    {
        uint32_t fps = profile->framerate ? profile->framerate : 30;
        GstClockTime frame_dur = GST_SECOND / fps;
        GST_BUFFER_PTS(buffer) = runtime->frames_pushed * frame_dur;
        GST_BUFFER_DURATION(buffer) = frame_dur;
    }

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(runtime->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        engine->total_frames_dropped++;
        goto done;
    }
    runtime->frames_pushed++;
done:
    g_mutex_unlock(&engine->lock);
    if (close_fd)
        close(dmabuf_fd);
#else
    (void)engine; (void)msg; (void)size;
    close(dmabuf_fd);
#endif
}

/* ── WebRTC signaling API ─────────────────────────────────────── */

/* Context for running ensure_profile + get_webrtc_offer on the main loop thread. */
static cJSON *get_webrtc_ice_candidates(sbs_preview_engine_t *engine,
                                         const char *profile_id);

typedef struct {
    sbs_preview_engine_t *engine;
    const char *profile_id;
    sbs_preview_color_mode_t color_mode;
    bool reference_color;
    sbs_preview_profile_t *profile;
    int rc;
    const char *offer_sdp;
    cJSON *ice_candidates;
    GMutex mutex;
    GCond cond;
    bool done;
} webrtc_start_ctx_t;

static gboolean webrtc_start_on_main_loop(gpointer user_data)
{
    webrtc_start_ctx_t *ctx = user_data;
    preview_runtime_t *runtime;
    sbs_preview_profile_t *profile;

    g_mutex_lock(&ctx->engine->lock);
    profile = preview_engine_get_profile_unlocked(ctx->engine, ctx->profile_id);
    if (profile) {
        profile->active_color_mode = ctx->engine->src_color_mode == SBS_PREVIEW_COLOR_MODE_HDR10
            ? ctx->color_mode : SBS_PREVIEW_COLOR_MODE_SDR;
        profile->reference_color = ctx->reference_color;
    }
    g_mutex_unlock(&ctx->engine->lock);

    /* ensure_profile on main loop thread */
    ctx->rc = sbs_preview_engine_ensure_profile(ctx->engine, ctx->profile_id, &ctx->profile);
    if (ctx->rc != SBS_OK) {
        goto done;
    }

    runtime = g_hash_table_lookup(ctx->engine->runtimes, ctx->profile_id);
    if (!runtime) {
        ctx->rc = SBS_ERR_NOT_FOUND;
        goto done;
    }

    /* The WebRTC pipeline runs on its own dedicated thread/context.
     * But on_negotiation_needed only fires after the first buffer push,
     * which requires the default main loop to dispatch frame distribution.
     * So we pump BOTH contexts during the wait. */
    LOG_I("WebRTC: waiting for offer (pumping main loop for frame delivery)...");
    for (int i = 0; i < 500 && !runtime->webrtc_ready; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
        if (i % 100 == 0 && i > 0) {
            LOG_D("WebRTC: still waiting for offer... (%d/500)", i);
        }
        g_usleep(10000); /* 10ms */
    }

    if (!runtime->webrtc_ready) {
        LOG_E("WebRTC: offer generation timed out (5s)");
        ctx->offer_sdp = NULL;
        ctx->rc = SBS_ERR_IO;
        goto done;
    }

    /* Wait for ICE candidates (up to 2s) */
    for (int i = 0; i < 200; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
        g_mutex_lock(&runtime->ice_mutex);
        guint n = runtime->ice_candidates ? runtime->ice_candidates->len : 0;
        g_mutex_unlock(&runtime->ice_mutex);
        if (n > 0) break;
        g_usleep(10000);
    }
    /* Grace period for more candidates */
    for (int i = 0; i < 20; i++) {
        g_main_context_iteration(g_main_context_default(), FALSE);
        g_usleep(10000);
    }

    ctx->offer_sdp = runtime->local_sdp;
    ctx->ice_candidates = get_webrtc_ice_candidates(
        ctx->engine, ctx->profile_id);

done:
    g_mutex_lock(&ctx->mutex);
    ctx->done = true;
    g_cond_signal(&ctx->cond);
    g_mutex_unlock(&ctx->mutex);
    return G_SOURCE_REMOVE;
}

int sbs_preview_engine_webrtc_start(sbs_preview_engine_t *engine,
                                    const char *profile_id,
                                    sbs_preview_color_mode_t color_mode,
                                    bool reference_color,
                                    sbs_preview_profile_t **out_profile,
                                    const char **out_sdp,
                                    cJSON **out_ice_candidates)
{
    webrtc_start_ctx_t ctx = {0};
    ctx.engine = engine;
    ctx.profile_id = profile_id;
    ctx.color_mode = color_mode;
    ctx.reference_color = reference_color;
    g_mutex_init(&ctx.mutex);
    g_cond_init(&ctx.cond);

    /* Schedule the work on the main loop thread so that:
     * 1. All profile/runtime state is modified on the main thread (same thread
     *    that reads it in consume_frame), avoiding race conditions.
     * 2. g_main_context_iteration can safely pump the main loop to deliver
     *    frames and dispatch on_negotiation_needed. */
    g_main_context_invoke(g_main_context_default(), webrtc_start_on_main_loop, &ctx);

    /* Wait for the main loop to finish (up to 10s generous timeout) */
    g_mutex_lock(&ctx.mutex);
    if (!ctx.done) {
        gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        while (!ctx.done) {
            if (!g_cond_wait_until(&ctx.cond, &ctx.mutex, deadline)) {
                LOG_E("WebRTC: main loop start timed out (10s)");
                ctx.rc = SBS_ERR_IO;
                break;
            }
        }
    }
    g_mutex_unlock(&ctx.mutex);

    g_mutex_clear(&ctx.mutex);
    g_cond_clear(&ctx.cond);

    if (out_profile) *out_profile = ctx.profile;
    if (out_sdp) *out_sdp = ctx.offer_sdp;
    if (out_ice_candidates) *out_ice_candidates = ctx.ice_candidates;
    return ctx.rc;
}

static cJSON *get_webrtc_ice_candidates(sbs_preview_engine_t *engine,
                                                     const char *profile_id)
{
    preview_runtime_t *runtime;
    cJSON *arr;
    if (!engine || !profile_id) return NULL;
    runtime = g_hash_table_lookup(engine->runtimes, profile_id);
    if (!runtime) return NULL;

    arr = cJSON_CreateArray();
    g_mutex_lock(&runtime->ice_mutex);
    if (runtime->ice_candidates) {
        for (guint i = 0; i < runtime->ice_candidates->len; i++) {
            const char *entry = g_ptr_array_index(runtime->ice_candidates, i);
            /* Format: "mline_index:candidate_string" */
            const char *colon = strchr(entry, ':');
            if (colon) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddNumberToObject(obj, "sdpMLineIndex", atoi(entry));
                cJSON_AddStringToObject(obj, "candidate", colon + 1);
                cJSON_AddItemToArray(arr, obj);
            }
        }
    }
    g_mutex_unlock(&runtime->ice_mutex);
    return arr;
}

int sbs_preview_engine_set_webrtc_answer(sbs_preview_engine_t *engine,
                                          const char *profile_id,
                                          const char *sdp_answer)
{
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    preview_runtime_t *runtime;
    webrtc_answer_task_t task = {0};

    if (!engine || !profile_id || !sdp_answer) return SBS_ERR_INVAL;
    g_mutex_lock(&engine->lock);
    runtime = g_hash_table_lookup(engine->runtimes, profile_id);
    if (!runtime || !runtime->webrtcbin) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_NOT_FOUND;
    }

    task.webrtcbin = gst_object_ref(runtime->webrtcbin);
    g_mutex_unlock(&engine->lock);
    task.sdp_answer = g_strdup(sdp_answer);
    g_mutex_init(&task.mutex);
    g_cond_init(&task.cond);

    g_main_context_invoke(runtime->context, set_webrtc_answer_on_context, &task);

    g_mutex_lock(&task.mutex);
    while (!task.done) {
        g_cond_wait(&task.cond, &task.mutex);
    }
    g_mutex_unlock(&task.mutex);

    gst_object_unref(task.webrtcbin);
    g_free(task.sdp_answer);
    g_mutex_clear(&task.mutex);
    g_cond_clear(&task.cond);
    return task.rc;
#else
    (void)engine; (void)profile_id; (void)sdp_answer;
    return SBS_ERR_INVAL;
#endif
}

int sbs_preview_engine_add_webrtc_ice(sbs_preview_engine_t *engine,
                                      const char *profile_id,
                                      unsigned int mline_index,
                                      const char *candidate)
{
#ifdef SBS_HAVE_GSTREAMER_PREVIEW
    preview_runtime_t *runtime;
    webrtc_ice_task_t *task;
    if (!engine || !profile_id || !candidate) return SBS_ERR_INVAL;
    g_mutex_lock(&engine->lock);
    runtime = g_hash_table_lookup(engine->runtimes, profile_id);
    if (!runtime || !runtime->webrtcbin) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_NOT_FOUND;
    }

    task = g_new0(webrtc_ice_task_t, 1);
    task->webrtcbin = gst_object_ref(runtime->webrtcbin);
    g_mutex_unlock(&engine->lock);
    task->mline_index = mline_index;
    task->candidate = g_strdup(candidate);
    g_main_context_invoke(runtime->context, add_webrtc_ice_on_context, task);
    return SBS_OK;
#else
    (void)engine; (void)profile_id; (void)mline_index; (void)candidate;
    return SBS_ERR_INVAL;
#endif
}

/* ── Profile config update ────────────────────────────────────── */

int sbs_preview_engine_update_profile_config(sbs_preview_engine_t *engine,
                                               const char *profile_id,
                                               uint32_t downscale_factor,
                                               uint32_t framerate,
                                               uint32_t bitrate_kbps)
{
    sbs_preview_profile_t *profile;
    bool changed = false;

    if (!engine || !profile_id)
        return SBS_ERR_INVAL;
    if (downscale_factor > 0 && !preview_downscale_factor_allowed(downscale_factor))
        return SBS_ERR_INVAL;

    g_mutex_lock(&engine->lock);
    profile = preview_engine_get_profile_unlocked(engine, profile_id);
    if (!profile) {
        g_mutex_unlock(&engine->lock);
        return SBS_ERR_NOT_FOUND;
    }

    if (downscale_factor > 0 && profile->requires_additional_encode) {
        uint32_t width = 0;
        uint32_t height = 0;
        compute_preview_size(engine->src_width, engine->src_height,
                             downscale_factor, &width, &height);
        changed = changed || profile->downscale_factor != downscale_factor ||
            profile->width != width || profile->height != height;
        profile->downscale_factor = downscale_factor;
        profile->width = width;
        profile->height = height;
    }
    if (framerate > 0) {
        if (profile->requires_additional_encode && framerate > 30) {
            LOG_I("preview profile %s capped at 30fps to protect 4K program output",
                  profile_id);
            framerate = 30;
        }
        changed = changed || profile->framerate != framerate;
        profile->framerate = framerate;
    }
    if (bitrate_kbps > 0) {
        changed = changed || profile->bitrate_kbps != bitrate_kbps;
        profile->bitrate_kbps = bitrate_kbps;
    }

    /* If the profile is active, tear down and restart the runtime so the
     * new settings take effect on the next WebRTC session start. */
    if (changed && profile->active && profile->requires_additional_encode) {
        LOG_I("preview config updated for %s — tearing down runtime for new settings",
              profile_id);
        g_hash_table_remove(engine->runtimes, profile->id);
        profile->active = false;
        profile->available = false;
        profile->viewer_count = 0;
    }

    LOG_I("preview profile %s config: %ux%u@%u downscale=%u bitrate=%u kbps",
          profile_id, profile->width, profile->height,
          profile->framerate, profile->downscale_factor, profile->bitrate_kbps);
    g_mutex_unlock(&engine->lock);
    return SBS_OK;
}

/* ── Telemetry / Serialization ────────────────────────────────── */

void sbs_preview_engine_collect_telemetry(const sbs_preview_engine_t *engine,
                                          sbs_preview_telemetry_t *telemetry)
{
    GHashTableIter iter;
    gpointer key, value;

    memset(telemetry, 0, sizeof(*telemetry));
    if (!engine) {
        return;
    }

    telemetry->total_frames_produced = engine->total_frames_produced;
    telemetry->total_frames_dropped = engine->total_frames_dropped;
    telemetry->avg_latency_ms = engine->latency_ms;
    telemetry->degraded = engine->degraded;

    g_hash_table_iter_init(&iter, engine->profiles);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_preview_profile_t *profile = value;
        if (profile->active) {
            telemetry->active_sessions++;
        }
        telemetry->total_sessions_created += profile->viewer_count;
    }
}

cJSON *sbs_preview_serialize_profile(const sbs_preview_profile_t *profile)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *resolution = cJSON_CreateObject();
    if (!profile) {
        cJSON_AddItemToObject(obj, "resolution", resolution);
        return obj;
    }

    cJSON_AddStringToObject(obj, "id", profile->id);
    cJSON_AddStringToObject(obj, "kind", kind_name(profile->kind));
    cJSON_AddStringToObject(obj, "transport", profile->transport);
    cJSON_AddStringToObject(obj, "codec", profile->codec);
    cJSON_AddStringToObject(obj, "container", profile->container);
    cJSON_AddStringToObject(obj, "latency_class", profile->latency_class);
    cJSON_AddNumberToObject(resolution, "width", profile->width);
    cJSON_AddNumberToObject(resolution, "height", profile->height);
    cJSON_AddItemToObject(obj, "resolution", resolution);
    cJSON_AddNumberToObject(obj, "downscale_factor", profile->downscale_factor);
    cJSON_AddNumberToObject(obj, "framerate", profile->framerate);
    cJSON_AddBoolToObject(obj, "hardware_decode_preferred", profile->hardware_decode_preferred);
    cJSON_AddBoolToObject(obj, "requires_additional_encode", profile->requires_additional_encode);
    cJSON_AddBoolToObject(obj, "available", profile->available);
    cJSON_AddBoolToObject(obj, "requestable", profile->requestable);
    cJSON_AddBoolToObject(obj, "active", profile->active);
    cJSON_AddStringToObject(obj, "color_mode",
                            profile->active_color_mode == SBS_PREVIEW_COLOR_MODE_HDR10
                                ? "hdr10" : "sdr_reference");
    cJSON_AddBoolToObject(obj, "reference_color", profile->reference_color);
    cJSON_AddNumberToObject(obj, "viewer_count", profile->viewer_count);
    cJSON_AddNumberToObject(obj, "bitrate_kbps", profile->bitrate_kbps);
    cJSON_AddStringToObject(obj, "stream_url", profile->stream_url ? profile->stream_url : "");
    return obj;
}

cJSON *sbs_preview_serialize_profile_catalog(const sbs_preview_engine_t *engine)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *available = cJSON_CreateArray();
    cJSON *requestable = cJSON_CreateArray();
    GHashTableIter iter;
    gpointer key, value;

    if (!engine) {
        cJSON_AddItemToObject(obj, "available_profiles", available);
        cJSON_AddItemToObject(obj, "requestable_profiles", requestable);
        return obj;
    }

    g_hash_table_iter_init(&iter, engine->profiles);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_preview_profile_t *profile = value;
        if (profile->available) {
            cJSON_AddItemToArray(available, sbs_preview_serialize_profile(profile));
        }
        if (profile->requestable) {
            cJSON_AddItemToArray(requestable, sbs_preview_serialize_profile(profile));
        }
    }

    cJSON_AddItemToObject(obj, "available_profiles", available);
    cJSON_AddItemToObject(obj, "requestable_profiles", requestable);
    return obj;
}

cJSON *sbs_preview_serialize_telemetry(const sbs_preview_engine_t *engine)
{
    sbs_preview_telemetry_t telemetry;
    cJSON *obj = cJSON_CreateObject();
    sbs_preview_engine_collect_telemetry(engine, &telemetry);
    cJSON_AddNumberToObject(obj, "active_sessions", telemetry.active_sessions);
    cJSON_AddNumberToObject(obj, "total_sessions_created", (double)telemetry.total_sessions_created);
    cJSON_AddNumberToObject(obj, "frames_produced", (double)telemetry.total_frames_produced);
    cJSON_AddNumberToObject(obj, "frames_dropped", (double)telemetry.total_frames_dropped);
    cJSON_AddNumberToObject(obj, "latency_ms", telemetry.avg_latency_ms);
    cJSON_AddBoolToObject(obj, "degraded", telemetry.degraded);
    return obj;
}
