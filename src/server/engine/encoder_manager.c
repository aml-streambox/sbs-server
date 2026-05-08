/*
 * SBS - StreamBox Broadcast System
 * Encoder Manager — direct Wave521 encoder + in-process GStreamer mux fanout
 *
 * Pipeline topology:
 *   Video: direct libvpcodec encode → appsrc(encoded) → queue → h265parse/h264parse → tee → [sink branches]
 *   Audio: appsrc(S16LE) → queue → audioconvert → audioresample → avenc_aac → aacparse → audio_tee → [sink branches]
 *
 * Each sink branch:
 *   video_tee.src_%u → queue → mux(mpegtsmux/flvmux) → sink(srtsink/rtmp2sink/filesink/fakesink)
 *   audio_tee.src_%u → queue → mux (linked to same muxer)
 *
 * Runs in-process on the main thread. Video frames are encoded directly via
 * libvpcodec from NV21 input and only encoded access units enter GStreamer.
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "enc-mgr"

#include "sbs/encoder_manager.h"
#include "sbs/direct_venc.h"
#include "sbs/log.h"

#include <string.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

/* ── Sink Branch State ────────────────────────────────────────── */

typedef struct sink_branch {
    char       *output_id;
    sbs_encoder_manager_t *manager;

    /* GStreamer elements (all owned by pipeline bin) */
    GstElement *video_queue;
    GstElement *video_pacer;
    GstElement *audio_queue;
    GstElement *muxer;
    GstElement *sink;

    /* Tee pad references */
    GstPad     *video_tee_pad;   /* request pad on video tee */
    GstPad     *audio_tee_pad;   /* request pad on audio tee */
    gulong      video_gate_probe_id;
    gulong      audio_gate_probe_id;
    gint        drop_until_keyframe;
    gint        srt_caller_count;

    /* Deep-copied config for potential restart */
    char       *sink_type;
    char       *srt_uri;
    uint32_t    srt_latency_ms;
    char       *rtmp_uri;
    char       *rtmp_passcode;
    char       *file_path;
} sink_branch_t;

#define SBS_ENCODER_PACER_DELAY_NS (250ULL * GST_MSECOND)

/* ── Encoder Manager State ────────────────────────────────────── */

struct sbs_encoder_manager {
    /* Pipeline elements */
    GstElement *pipeline;
    GstElement *video_appsrc;
    GstElement *audio_appsrc;
    GstElement *parser;
    GstElement *video_tee;
    GstElement *audio_tee;

    /* Direct video encoder */
    sbs_direct_venc_t *video_encoder;

    /* A fakesink on the tee to keep data flowing when no branches exist */
    GstElement *video_fakesink;
    GstElement *audio_fakesink;
    GstPad     *video_fake_tee_pad;
    GstPad     *audio_fake_tee_pad;

    /* Sink branches: output_id → sink_branch_t* */
    GHashTable *branches;

    /* Video format */
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;

    /* Encoder config (deep-copied) */
    char    *codec;
    uint32_t bitrate_kbps;
    uint32_t gop_size;
    int32_t  gop_pattern;       /* -1 = unset, 0 = IPP, 1 = IBP, etc. */
    int32_t  rc_mode;           /* 0 = VBR (default), 1 = CBR */
    char    *encoder_override;

    /* Frame counters */
    uint64_t frames_pushed;
    uint64_t frames_dropped;
    uint64_t encoded_bytes;
    uint64_t audio_buffers_pushed;

    /* Parser name cache */
    char    *parser_name;  /* "h265parse" or "h264parse" */
    bool     is_h265;
    bool     force_next_idr;
    bool     hdr10;
    bool     video_time_origin_valid;
    uint64_t video_pts_origin_ns;
    uint64_t video_running_origin_ns;
    bool     audio_time_origin_valid;
    uint64_t audio_pts_origin_ns;
    uint64_t audio_running_origin_ns;

    bool     pipeline_active;
    pthread_mutex_t pipeline_mutex;
};

/* ── Forward Declarations ─────────────────────────────────────── */

static int  ensure_pipeline(sbs_encoder_manager_t *mgr);
static void teardown_pipeline(sbs_encoder_manager_t *mgr);
static void sink_branch_free(gpointer data);
static int  link_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch);
static void unlink_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch);

static void on_srt_caller_added(GstElement *sink,
                                gint caller_id,
                                gpointer address,
                                gpointer user_data)
{
    (void)sink;
    (void)address;

    sink_branch_t *branch = user_data;
    sbs_encoder_manager_t *mgr = branch ? branch->manager : NULL;
    if (!mgr)
        return;

    g_atomic_int_inc(&branch->srt_caller_count);
    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);

    pthread_mutex_lock(&mgr->pipeline_mutex);
    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
    mgr->force_next_idr = true;
    pthread_mutex_unlock(&mgr->pipeline_mutex);

    LOG_I("SRT caller %d connected; scheduled IDR", caller_id);
}

static void on_srt_caller_removed(GstElement *sink,
                                  gint caller_id,
                                  gpointer address,
                                  gpointer user_data)
{
    (void)sink;
    (void)address;

    sink_branch_t *branch = user_data;
    if (!branch)
        return;

    gint old_count = g_atomic_int_add(&branch->srt_caller_count, -1);
    gint new_count = old_count > 0 ? old_count - 1 : 0;
    if (old_count <= 0)
        g_atomic_int_set(&branch->srt_caller_count, 0);
    if (new_count == 0)
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);

    LOG_I("SRT caller %d disconnected; callers=%d", caller_id, new_count);
}

static gboolean on_srt_caller_connecting(GstElement *sink,
                                          gpointer address,
                                          gchar *stream_id,
                                          gpointer user_data)
{
    (void)sink;
    (void)address;
    (void)stream_id;

    sink_branch_t *branch = user_data;
    sbs_encoder_manager_t *mgr = branch ? branch->manager : NULL;
    if (!mgr)
        return TRUE;

    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);

    pthread_mutex_lock(&mgr->pipeline_mutex);
    mgr->force_next_idr = true;
    pthread_mutex_unlock(&mgr->pipeline_mutex);

    LOG_I("SRT caller connecting; scheduled IDR");
    return TRUE;
}

static GstPadProbeReturn srt_keyframe_gate_probe(GstPad *pad,
                                                  GstPadProbeInfo *info,
                                                  gpointer user_data)
{
    (void)pad;

    sink_branch_t *branch = user_data;
    GstBuffer *buffer;

    if (!branch || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 &&
        g_atomic_int_get(&branch->srt_caller_count) <= 0) {
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        return GST_PAD_PROBE_DROP;
    }

    if (!g_atomic_int_get(&branch->drop_until_keyframe))
        return GST_PAD_PROBE_OK;

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;

    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
        return GST_PAD_PROBE_DROP;

    g_atomic_int_set(&branch->drop_until_keyframe, FALSE);
    LOG_I("SRT branch '%s' resumed at keyframe", branch->output_id);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn srt_audio_gate_probe(GstPad *pad,
                                              GstPadProbeInfo *info,
                                              gpointer user_data)
{
    (void)pad;

    sink_branch_t *branch = user_data;

    if (!branch || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 &&
        (g_atomic_int_get(&branch->srt_caller_count) <= 0 ||
         g_atomic_int_get(&branch->drop_until_keyframe))) {
        return GST_PAD_PROBE_DROP;
    }

    return GST_PAD_PROBE_OK;
}

/* ── GStreamer Init ────────────────────────────────────────────── */

static void ensure_gstreamer_ready(void)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        gst_init(NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

/* ── H.265 Stream Sanitizer (pad probe) ──────────────────────── */

static GstPadProbeReturn
h265_stream_sanitize_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    (void)user_data;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_DROP;

    gboolean found_vps = FALSE;
    for (gsize i = 0; i + 4 < map.size; i++) {
        if ((map.data[i] == 0x00 && map.data[i+1] == 0x00 && map.data[i+2] == 0x01) ||
            (i + 5 < map.size && map.data[i] == 0x00 && map.data[i+1] == 0x00 &&
             map.data[i+2] == 0x00 && map.data[i+3] == 0x01)) {
            gsize nal_start = (map.data[i+2] == 0x01) ? i + 3 : i + 4;
            if (nal_start < map.size) {
                uint8_t nal_type = (map.data[nal_start] >> 1) & 0x3F;
                if (nal_type == 32) { /* VPS_NUT */
                    found_vps = TRUE;
                    break;
                }
            }
        }
    }

    gsize buf_size = map.size;
    gst_buffer_unmap(buffer, &map);

    if (found_vps) {
        LOG_I("h265 sanitizer: VPS found, removing probe — stream is clean");
        return GST_PAD_PROBE_REMOVE;
    }

    LOG_W("h265 sanitizer: dropping pre-VPS buffer (%zu bytes)", (size_t)buf_size);
    return GST_PAD_PROBE_DROP;
}

/* ── Pipeline Bus Handler ─────────────────────────────────────── */

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void)bus;
    (void)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOG_E("encoder pipeline error from %s: %s (%s)",
              GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        LOG_W("encoder pipeline warning from %s: %s (%s)",
              GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_IS_PIPELINE(msg->src)) {
            GstState old_state, new_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
            LOG_D("encoder pipeline state: %s -> %s",
                  gst_element_state_get_name(old_state),
                  gst_element_state_get_name(new_state));
        }
        break;
    case GST_MESSAGE_EOS:
        LOG_W("encoder pipeline EOS");
        break;
    default:
        break;
    }
    return TRUE;
}

/* ── Pipeline Construction ────────────────────────────────────── */

static GstClockTime pipeline_running_time(sbs_encoder_manager_t *mgr)
{
    GstClockTime now;
    GstClockTime base;
    GstClock *clock;

    if (!mgr || !mgr->pipeline)
        return 0;

    clock = gst_element_get_clock(mgr->pipeline);
    if (!clock)
        return 0;

    now = gst_clock_get_time(clock);
    base = gst_element_get_base_time(mgr->pipeline);
    gst_object_unref(clock);

    if (!GST_CLOCK_TIME_IS_VALID(now) || !GST_CLOCK_TIME_IS_VALID(base) || now < base)
        return 0;

    return now - base;
}

static GstClockTime normalize_video_time(sbs_encoder_manager_t *mgr,
                                          uint64_t timestamp_ns)
{
    uint64_t rel;

    if (!mgr->video_time_origin_valid) {
        mgr->video_pts_origin_ns = timestamp_ns;
        mgr->video_running_origin_ns = pipeline_running_time(mgr) +
                                       SBS_ENCODER_PACER_DELAY_NS;
        mgr->video_time_origin_valid = true;
        LOG_I("encoded video clock origin: pts=%luns running=%luns",
              (unsigned long)mgr->video_pts_origin_ns,
              (unsigned long)mgr->video_running_origin_ns);
    }

    rel = timestamp_ns >= mgr->video_pts_origin_ns
        ? timestamp_ns - mgr->video_pts_origin_ns
        : 0;
    return (GstClockTime)(mgr->video_running_origin_ns + rel);
}

static GstClockTime normalize_audio_time(sbs_encoder_manager_t *mgr,
                                          uint64_t timestamp_ns)
{
    uint64_t rel;

    if (timestamp_ns == UINT64_MAX) {
        return pipeline_running_time(mgr) + SBS_ENCODER_PACER_DELAY_NS;
    }

    if (!mgr->audio_time_origin_valid) {
        mgr->audio_pts_origin_ns = timestamp_ns;
        mgr->audio_running_origin_ns = pipeline_running_time(mgr) +
                                       SBS_ENCODER_PACER_DELAY_NS;
        mgr->audio_time_origin_valid = true;
        LOG_I("encoded audio clock origin: pts=%luns running=%luns",
              (unsigned long)mgr->audio_pts_origin_ns,
              (unsigned long)mgr->audio_running_origin_ns);
    }

    rel = timestamp_ns >= mgr->audio_pts_origin_ns
        ? timestamp_ns - mgr->audio_pts_origin_ns
        : 0;
    return (GstClockTime)(mgr->audio_running_origin_ns + rel);
}

static void push_encoded_packet(sbs_encoder_manager_t *mgr,
                                const sbs_direct_venc_packet_t *packet)
{
    if (!mgr || !mgr->video_appsrc || !packet || !packet->data || packet->size == 0)
        return;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
    if (!buffer) {
        mgr->frames_dropped++;
        return;
    }

    gst_buffer_fill(buffer, 0, packet->data, packet->size);
    GST_BUFFER_PTS(buffer) = normalize_video_time(mgr, packet->pts_ns);
    GST_BUFFER_DTS(buffer) = (packet->dts_ns == UINT64_MAX)
        ? GST_CLOCK_TIME_NONE
        : normalize_video_time(mgr, packet->dts_ns);
    GST_BUFFER_DURATION(buffer) = packet->duration_ns;

    /* Mark keyframes so downstream parser/muxer knows. */
    if (packet->is_keyframe)
        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    else
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(mgr->video_appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        mgr->frames_dropped++;
        if (mgr->frames_dropped % 100 == 1) {
            LOG_W("encoded video appsrc push failed: %s (dropped: %lu)",
                  gst_flow_get_name(ret), (unsigned long)mgr->frames_dropped);
        }
    }
}

static int ensure_pipeline(sbs_encoder_manager_t *mgr)
{
    if (mgr->pipeline_active) return SBS_OK;

    ensure_gstreamer_ready();

    sbs_direct_venc_config_t venc_cfg = {
        .codec = mgr->codec,
        .width = mgr->width,
        .height = mgr->height,
        .fps_num = mgr->fps_num,
        .fps_den = mgr->fps_den,
        .bitrate_kbps = mgr->bitrate_kbps,
        .gop_size = mgr->gop_size,
        .gop_pattern = mgr->gop_pattern,
        .rc_mode = mgr->rc_mode,
        .hdr10 = mgr->hdr10,
    };

    mgr->video_encoder = sbs_direct_venc_new(&venc_cfg);
    if (!mgr->video_encoder) {
        LOG_E("failed to create direct video encoder");
        return SBS_ERR_IO;
    }

    const char *parser_name = (!mgr->codec || strcmp(mgr->codec, "h265") == 0)
        ? "h265parse"
        : "h264parse";
    g_free(mgr->parser_name);
    mgr->parser_name = g_strdup(parser_name);
    mgr->is_h265 = (!mgr->codec || strcmp(mgr->codec, "h265") == 0);

    GstElement *pipeline    = gst_pipeline_new("encoder-pipeline");
    GstElement *appsrc      = gst_element_factory_make("appsrc", "video_src");
    GstElement *q1          = gst_element_factory_make("queue", "vq1");
    GstElement *parser      = gst_element_factory_make(parser_name, "parser");
    GstElement *video_tee   = gst_element_factory_make("tee", "video_tee");

    /* Audio chain */
    GstElement *audio_appsrc  = gst_element_factory_make("appsrc", "audio_src");
    GstElement *aq1           = gst_element_factory_make("queue", "aq1");
    GstElement *aconv         = gst_element_factory_make("audioconvert", "aconv");
    GstElement *aresample     = gst_element_factory_make("audioresample", "aresample");
    GstElement *aenc          = gst_element_factory_make("avenc_aac", "aenc");
    if (!aenc) aenc           = gst_element_factory_make("voaacenc", "aenc");
    GstElement *aparse        = gst_element_factory_make("aacparse", "aparse");
    GstElement *audio_tee     = gst_element_factory_make("tee", "audio_tee");

    /* Fakesinks to keep tees happy when no branches exist */
    GstElement *video_fakesink = gst_element_factory_make("fakesink", "vfakesink");
    GstElement *audio_fakesink = gst_element_factory_make("fakesink", "afakesink");

    if (!pipeline || !appsrc || !q1 || !parser || !video_tee ||
        !audio_appsrc || !aq1 || !aconv || !aresample || !aenc || !aparse || !audio_tee ||
        !video_fakesink || !audio_fakesink) {
        LOG_E("failed to create one or more pipeline elements");
        /* Cleanup refs */
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (q1) gst_object_unref(q1);
        if (parser) gst_object_unref(parser);
        if (video_tee) gst_object_unref(video_tee);
        if (audio_appsrc) gst_object_unref(audio_appsrc);
        if (aq1) gst_object_unref(aq1);
        if (aconv) gst_object_unref(aconv);
        if (aresample) gst_object_unref(aresample);
        if (aenc) gst_object_unref(aenc);
        if (aparse) gst_object_unref(aparse);
        if (audio_tee) gst_object_unref(audio_tee);
        if (video_fakesink) gst_object_unref(video_fakesink);
        if (audio_fakesink) gst_object_unref(audio_fakesink);
        sbs_direct_venc_free(mgr->video_encoder);
        mgr->video_encoder = NULL;
        return SBS_ERR_IO;
    }

    /* Configure encoded video appsrc */
    GstCaps *vcaps = gst_caps_new_simple(mgr->is_h265 ? "video/x-h265" : "video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    g_object_set(appsrc,
        "caps",         vcaps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    (guint64)(mgr->bitrate_kbps * 1000),
        NULL);
    gst_caps_unref(vcaps);

    /* Configure audio appsrc */
    GstCaps *acaps = gst_caps_new_simple("audio/x-raw",
        "format",  G_TYPE_STRING, "S16LE",
        "rate",    G_TYPE_INT, 48000,
        "channels", G_TYPE_INT, 2,
        "layout",  G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(audio_appsrc,
        "caps",         acaps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    48000 * 2 * 2 * 2,
        NULL);
    gst_caps_unref(acaps);

    /* Configure queue */
    g_object_set(q1,
        "max-size-buffers", (guint)15,
        "max-size-time",    (guint64)0,
        "max-size-bytes",   (guint)0,
        NULL);

    /* Repeat parameter sets on every IDR so late SRT callers can join on
     * the first gated keyframe without decoder PPS/VPS misses. */
    if (mgr->is_h265) {
        g_object_set(parser,
            "config-interval",  (gint)-1,
            "disable-passthrough", TRUE,
            NULL);
    } else {
        g_object_set(parser,
            "config-interval",  (gint)-1,
            "disable-passthrough", TRUE,
            NULL);
    }

    /* Configure audio queue */
    g_object_set(aq1,
        "max-size-buffers", 0,
        "max-size-bytes",   0,
        "max-size-time",    (guint64)(2 * GST_SECOND),
        "leaky",            2,
        NULL);

    /* Configure audio encoder */
    if (aenc) g_object_set(aenc, "bitrate", 128000, NULL);

    /* Configure tee to allow-not-linked so branches can be added/removed */
    g_object_set(video_tee, "allow-not-linked", TRUE, NULL);
    g_object_set(audio_tee, "allow-not-linked", TRUE, NULL);

    /* Configure fakesinks */
    g_object_set(video_fakesink, "sync", FALSE, "async", FALSE, NULL);
    g_object_set(audio_fakesink, "sync", FALSE, "async", FALSE, NULL);

    /* Add all to pipeline */
    gst_bin_add_many(GST_BIN(pipeline),
        appsrc, q1, parser, video_tee,
        audio_appsrc, aq1, aconv, aresample, aenc, aparse, audio_tee,
        video_fakesink, audio_fakesink,
        NULL);

    /* Link video chain: encoded appsrc → q1 → parser → video_tee */
    if (!gst_element_link_many(appsrc, q1, parser, video_tee, NULL)) {
        LOG_E("failed to link encoded video chain");
        gst_object_unref(pipeline);
        sbs_direct_venc_free(mgr->video_encoder);
        mgr->video_encoder = NULL;
        return SBS_ERR_IO;
    }

    /* Link audio chain: audio_appsrc → aq1 → aconv → aresample → aenc → aparse → audio_tee */
    if (!gst_element_link_many(audio_appsrc, aq1, aconv, aresample, aenc, aparse, audio_tee, NULL)) {
        LOG_E("failed to link audio encode chain");
        gst_object_unref(pipeline);
        sbs_direct_venc_free(mgr->video_encoder);
        mgr->video_encoder = NULL;
        return SBS_ERR_IO;
    }

    /* Link fakesinks to tees (so data flows even with no branches) */
    {
        GstPad *vfake_tee_pad = gst_element_request_pad_simple(video_tee, "src_%u");
        GstPad *vfake_sink_pad = gst_element_get_static_pad(video_fakesink, "sink");
        gst_pad_link(vfake_tee_pad, vfake_sink_pad);
        gst_object_unref(vfake_sink_pad);
        mgr->video_fake_tee_pad = vfake_tee_pad;

        GstPad *afake_tee_pad = gst_element_request_pad_simple(audio_tee, "src_%u");
        GstPad *afake_sink_pad = gst_element_get_static_pad(audio_fakesink, "sink");
        gst_pad_link(afake_tee_pad, afake_sink_pad);
        gst_object_unref(afake_sink_pad);
        mgr->audio_fake_tee_pad = afake_tee_pad;
    }

    /* Install H.265 sanitizer probe */
    if (mgr->is_h265) {
        GstPad *src_pad = gst_element_get_static_pad(parser, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                              h265_stream_sanitize_probe, NULL, NULL);
            gst_object_unref(src_pad);
            LOG_I("h265 stream sanitizer probe installed");
        }
    }

    /* Install bus watch */
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, on_bus_message, mgr);
    gst_object_unref(bus);

    /* Start pipeline */
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_E("encoder pipeline failed to enter PLAYING");
        gst_object_unref(pipeline);
        sbs_direct_venc_free(mgr->video_encoder);
        mgr->video_encoder = NULL;
        return SBS_ERR_IO;
    }

    /* Store references */
    mgr->pipeline       = pipeline;
    mgr->video_appsrc   = gst_object_ref(appsrc);
    mgr->audio_appsrc   = gst_object_ref(audio_appsrc);
    mgr->parser         = parser;
    mgr->video_tee      = video_tee;
    mgr->audio_tee      = audio_tee;
    mgr->video_fakesink = video_fakesink;
    mgr->audio_fakesink = audio_fakesink;
    mgr->pipeline_active = true;
    mgr->frames_pushed   = 0;
    mgr->audio_buffers_pushed = 0;
    mgr->force_next_idr = true;
    mgr->video_time_origin_valid = false;
    mgr->video_pts_origin_ns = 0;
    mgr->video_running_origin_ns = 0;
    mgr->audio_time_origin_valid = false;
    mgr->audio_pts_origin_ns = 0;
    mgr->audio_running_origin_ns = 0;

    LOG_I("encoder pipeline PLAYING: %ux%u@%u/%u codec=%s bitrate=%u",
          mgr->width, mgr->height, mgr->fps_num, mgr->fps_den,
          mgr->codec ? mgr->codec : "h265", mgr->bitrate_kbps);

    return SBS_OK;
}

static void teardown_pipeline(sbs_encoder_manager_t *mgr)
{
    if (!mgr->pipeline) return;

    LOG_I("tearing down encoder pipeline (pushed %lu frames)", (unsigned long)mgr->frames_pushed);

    LOG_I("teardown: sending EOS...");
    if (mgr->video_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(mgr->video_appsrc));
    }
    if (mgr->audio_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(mgr->audio_appsrc));
    }

    LOG_I("teardown: draining bus...");
    GstBus *drain_bus = gst_element_get_bus(mgr->pipeline);
    GstMessage *eos_msg = gst_bus_timed_pop_filtered(drain_bus,
        2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    LOG_I("teardown: drain returned msg_type=%s",
          eos_msg ? GST_MESSAGE_TYPE_NAME(eos_msg) : "<timeout>");
    if (eos_msg) gst_message_unref(eos_msg);
    LOG_I("teardown: unreffed eos_msg");
    gst_object_unref(drain_bus);
    LOG_I("teardown: unreffed drain_bus");

    LOG_I("teardown: setting pipeline to NULL...");
    gst_element_set_state(mgr->pipeline, GST_STATE_NULL);
    LOG_I("teardown: pipeline is NULL");

    /* Remove bus watch */
    GstBus *bus = gst_element_get_bus(mgr->pipeline);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    LOG_I("teardown: removed bus watch");

    /* Release tee request pads */
    if (mgr->video_fake_tee_pad) {
        LOG_I("teardown: releasing video fake tee pad");
        gst_element_release_request_pad(mgr->video_tee, mgr->video_fake_tee_pad);
        gst_object_unref(mgr->video_fake_tee_pad);
        mgr->video_fake_tee_pad = NULL;
    }
    if (mgr->audio_fake_tee_pad) {
        LOG_I("teardown: releasing audio fake tee pad");
        gst_element_release_request_pad(mgr->audio_tee, mgr->audio_fake_tee_pad);
        gst_object_unref(mgr->audio_fake_tee_pad);
        mgr->audio_fake_tee_pad = NULL;
    }

    if (mgr->video_appsrc) {
        LOG_I("teardown: unreffing video_appsrc");
        gst_object_unref(mgr->video_appsrc);
        mgr->video_appsrc = NULL;
    }
    if (mgr->audio_appsrc) {
        LOG_I("teardown: unreffing audio_appsrc");
        gst_object_unref(mgr->audio_appsrc);
        mgr->audio_appsrc = NULL;
    }

    LOG_I("teardown: unreffing pipeline");
    gst_object_unref(mgr->pipeline);
    mgr->pipeline        = NULL;
    mgr->parser          = NULL;
    mgr->video_tee       = NULL;
    mgr->audio_tee       = NULL;
    mgr->video_fakesink  = NULL;
    mgr->audio_fakesink  = NULL;
    mgr->pipeline_active = false;

    if (mgr->video_encoder) {
        LOG_I("teardown: freeing direct venc");
        sbs_direct_venc_free(mgr->video_encoder);
        mgr->video_encoder = NULL;
        LOG_I("teardown: direct venc freed");
    }
}

/* ── Sink Branch Management ───────────────────────────────────── */

static void sink_branch_free(gpointer data)
{
    sink_branch_t *branch = data;
    if (!branch) return;

    g_free(branch->output_id);
    g_free(branch->sink_type);
    g_free(branch->srt_uri);
    g_free(branch->rtmp_uri);
    g_free(branch->rtmp_passcode);
    g_free(branch->file_path);

    /* Tee pads and elements are released in unlink_sink_branch() before free */

    g_free(branch);
}

static char *build_rtmp_location(const char *rtmp_uri, const char *rtmp_passcode)
{
    if (!rtmp_uri || !*rtmp_uri)
        return NULL;

    if (!rtmp_passcode || !*rtmp_passcode)
        return g_strdup(rtmp_uri);

    while (*rtmp_passcode == '/')
        rtmp_passcode++;
    if (!*rtmp_passcode)
        return g_strdup(rtmp_uri);

    const char *sep = rtmp_uri[strlen(rtmp_uri) - 1] == '/' ? "" : "/";
    return g_strconcat(rtmp_uri, sep, rtmp_passcode, NULL);
}

static GstElement *create_muxer(const char *codec, const char *sink_type,
                                 const char **out_format)
{
    bool is_rtmp = (sink_type && strcmp(sink_type, "rtmp") == 0);
    bool is_h264 = (codec && strcmp(codec, "h264") == 0);

    *out_format = "mpegts";
    GstElement *muxer;
    if (is_rtmp && is_h264) {
        muxer = gst_element_factory_make("flvmux", NULL);
        if (muxer) g_object_set(muxer, "streamable", TRUE, NULL);
        *out_format = "flv";
    } else {
        muxer = gst_element_factory_make("mpegtsmux", NULL);
        if (muxer) {
            g_object_set(muxer,
                "alignment", (gint)7,
                "latency",   (guint64)100000000,
                NULL);
        }
    }
    return muxer;
}

static GstElement *create_sink(const sbs_sink_branch_config_t *config,
                                 const char *muxer_format)
{
    GstElement *sink = NULL;
    const char *sink_type = config->sink_type;

    if (sink_type && strcmp(sink_type, "fakesink") == 0) {
        sink = gst_element_factory_make("fakesink", NULL);
        if (sink) {
            g_object_set(sink, "sync", FALSE, NULL);
            LOG_I("fakesink selected for output");
        }
    } else if (!sink_type || strcmp(sink_type, "srt") == 0) {
        if (config->srt_uri && strlen(config->srt_uri) > 0) {
            sink = gst_element_factory_make("srtsink", NULL);
            if (sink) {
                uint32_t latency_ms = config->srt_latency_ms > 0
                    ? config->srt_latency_ms : 600;
                g_object_set(sink,
                    "uri",                 config->srt_uri,
                    "wait-for-connection", FALSE,
                    "latency",             (gint)latency_ms,
                    "sync",                FALSE,
                    "async",               FALSE,
                    NULL);
                LOG_I("SRT sink: %s latency=%ums sync=0", config->srt_uri, latency_ms);
            }
        } else {
            LOG_E("SRT sink requested without srt_uri");
        }
    } else if (strcmp(sink_type, "rtmp") == 0) {
        if (config->rtmp_uri && strlen(config->rtmp_uri) > 0) {
            char *location = build_rtmp_location(config->rtmp_uri, config->rtmp_passcode);
            if (muxer_format && strcmp(muxer_format, "flv") == 0) {
                sink = gst_element_factory_make("rtmp2sink", NULL);
                if (!sink) sink = gst_element_factory_make("rtmpsink", NULL);
            } else {
                sink = gst_element_factory_make("rtmpsink", NULL);
                if (!sink) sink = gst_element_factory_make("rtmp2sink", NULL);
            }
            if (sink) {
                g_object_set(sink, "location", location ? location : config->rtmp_uri, "sync", FALSE, NULL);
                LOG_I("RTMP sink: %s%s", config->rtmp_uri,
                      config->rtmp_passcode && *config->rtmp_passcode ? " (passcode set)" : "");
            }
            g_free(location);
        } else {
            LOG_E("RTMP sink requested without rtmp_uri");
        }
    } else if (strcmp(sink_type, "file") == 0) {
        if (config->file_path && strlen(config->file_path) > 0) {
            sink = gst_element_factory_make("filesink", NULL);
            if (sink) {
                g_object_set(sink, "location", config->file_path, "sync", FALSE, NULL);
                LOG_I("File sink: %s", config->file_path);
            }
        } else {
            LOG_E("file sink requested without file_path");
        }
    } else {
        LOG_E("unsupported sink_type='%s'", sink_type);
    }

    if (!sink) {
        LOG_E("failed to create requested output sink '%s'", sink_type ? sink_type : "srt");
    }

    return sink;
}

static int link_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    GstElement *vqueue = gst_element_factory_make("queue", NULL);
    GstElement *vpacer = gst_element_factory_make("identity", NULL);
    GstElement *aqueue = gst_element_factory_make("queue", NULL);
    const char *muxer_format = NULL;
    GstElement *muxer  = create_muxer(mgr->codec, branch->sink_type, &muxer_format);

    sbs_sink_branch_config_t cfg = {
        .output_id    = branch->output_id,
        .sink_type    = branch->sink_type,
        .srt_uri      = branch->srt_uri,
        .srt_latency_ms = branch->srt_latency_ms,
        .rtmp_uri     = branch->rtmp_uri,
        .rtmp_passcode = branch->rtmp_passcode,
        .file_path    = branch->file_path,
    };
    GstElement *sink = create_sink(&cfg, muxer_format);

    branch->manager = mgr;
    branch->drop_until_keyframe = TRUE;
    branch->srt_caller_count = 0;

    if (!vqueue || !vpacer || !aqueue || !muxer || !sink) {
        LOG_E("failed to create sink branch elements for '%s'", branch->output_id);
        if (vqueue) gst_object_unref(vqueue);
        if (vpacer) gst_object_unref(vpacer);
        if (aqueue) gst_object_unref(aqueue);
        if (muxer) gst_object_unref(muxer);
        if (sink) gst_object_unref(sink);
        return SBS_ERR_IO;
    }

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        g_signal_connect(sink, "caller-connecting",
                         G_CALLBACK(on_srt_caller_connecting), branch);
        g_signal_connect(sink, "caller-added",
                         G_CALLBACK(on_srt_caller_added), branch);
        g_signal_connect(sink, "caller-removed",
                         G_CALLBACK(on_srt_caller_removed), branch);
        g_object_set(sink, "authentication", TRUE, NULL);
    }

    /* Configure video queue */
    g_object_set(vqueue,
        "max-size-buffers", (guint)120,
        "max-size-time",    (guint64)(2 * GST_SECOND),
        "max-size-bytes",   (guint)0,
        NULL);

    g_object_set(aqueue,
        "max-size-buffers", (guint)0,
        "max-size-time",    (guint64)(2 * GST_SECOND),
        "max-size-bytes",   (guint)0,
        "leaky",            2,
        NULL);

    g_object_set(vpacer,
        "sync", TRUE,
        NULL);

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        GstPad *gate_pad = gst_element_get_static_pad(vpacer, "sink");
        if (gate_pad) {
            branch->video_gate_probe_id = gst_pad_add_probe(
                gate_pad, GST_PAD_PROBE_TYPE_BUFFER,
                srt_keyframe_gate_probe, branch, NULL);
            gst_object_unref(gate_pad);
        }

        gate_pad = gst_element_get_static_pad(aqueue, "sink");
        if (gate_pad) {
            branch->audio_gate_probe_id = gst_pad_add_probe(
                gate_pad, GST_PAD_PROBE_TYPE_BUFFER,
                srt_audio_gate_probe, branch, NULL);
            gst_object_unref(gate_pad);
        }
    }

    /* Add to pipeline */
    gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);

    /* Link: vqueue → timestamp pacer → muxer → sink */
    if (!gst_element_link_many(vqueue, vpacer, muxer, sink, NULL)) {
        LOG_E("failed to link video branch for '%s'", branch->output_id);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }

    if (!gst_element_link(aqueue, muxer)) {
        LOG_E("failed to link audio branch into muxer for '%s'", branch->output_id);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }

    /* Sync state with parent pipeline */
    gst_element_sync_state_with_parent(vqueue);
    gst_element_sync_state_with_parent(vpacer);
    gst_element_sync_state_with_parent(aqueue);
    gst_element_sync_state_with_parent(muxer);
    gst_element_sync_state_with_parent(sink);

    /* Request pad from video tee and link */
    GstPad *vtee_pad = gst_element_request_pad_simple(mgr->video_tee, "src_%u");
    GstPad *vq_sink  = gst_element_get_static_pad(vqueue, "sink");
    if (!vtee_pad || !vq_sink) {
        LOG_E("failed to request video branch pads for '%s'", branch->output_id);
        if (vtee_pad) gst_object_unref(vtee_pad);
        if (vq_sink) gst_object_unref(vq_sink);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    if (gst_pad_link(vtee_pad, vq_sink) != GST_PAD_LINK_OK) {
        LOG_E("failed to link video tee to queue for '%s'", branch->output_id);
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        gst_object_unref(vq_sink);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    gst_object_unref(vq_sink);

    GstPad *atee_pad = gst_element_request_pad_simple(mgr->audio_tee, "src_%u");
    GstPad *aq_sink  = gst_element_get_static_pad(aqueue, "sink");
    if (!atee_pad || !aq_sink) {
        GstPad *vq_sink2 = gst_element_get_static_pad(vqueue, "sink");
        LOG_E("failed to request audio branch pads for '%s'", branch->output_id);
        if (vq_sink2) {
            gst_pad_unlink(vtee_pad, vq_sink2);
            gst_object_unref(vq_sink2);
        }
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        if (atee_pad) {
            gst_element_release_request_pad(mgr->audio_tee, atee_pad);
            gst_object_unref(atee_pad);
        }
        if (aq_sink) gst_object_unref(aq_sink);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    if (gst_pad_link(atee_pad, aq_sink) != GST_PAD_LINK_OK) {
        GstPad *vq_sink2 = gst_element_get_static_pad(vqueue, "sink");
        LOG_E("failed to link audio tee to queue for '%s'", branch->output_id);
        if (vq_sink2) {
            gst_pad_unlink(vtee_pad, vq_sink2);
            gst_object_unref(vq_sink2);
        }
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        gst_element_release_request_pad(mgr->audio_tee, atee_pad);
        gst_object_unref(atee_pad);
        gst_object_unref(aq_sink);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    gst_object_unref(aq_sink);

    /* Store references in branch. */
    branch->video_queue    = vqueue;
    branch->video_pacer    = vpacer;
    branch->audio_queue    = aqueue;
    branch->muxer          = muxer;
    branch->sink           = sink;
    branch->video_tee_pad  = vtee_pad;
    branch->audio_tee_pad  = atee_pad;

    LOG_I("sink branch '%s' linked (type=%s)", branch->output_id,
          branch->sink_type ? branch->sink_type : "srt");

    return SBS_OK;
}

static void unlink_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    if (!mgr->pipeline || !branch->video_queue) return;

    LOG_I("unlinking sink branch '%s'", branch->output_id);

    GstPad *vq_sink = NULL;
    GstPad *aq_sink = NULL;
    if (branch->video_queue) {
        vq_sink = gst_element_get_static_pad(branch->video_queue, "sink");
    }
    if (branch->audio_queue) {
        aq_sink = gst_element_get_static_pad(branch->audio_queue, "sink");
    }

    if (branch->video_tee_pad && vq_sink) {
        LOG_I("unlink branch '%s': unlink video tee pad", branch->output_id);
        gst_pad_unlink(branch->video_tee_pad, vq_sink);
    }
    if (branch->audio_tee_pad && aq_sink) {
        LOG_I("unlink branch '%s': unlink audio tee pad", branch->output_id);
        gst_pad_unlink(branch->audio_tee_pad, aq_sink);
    }
    if (vq_sink) {
        gst_object_unref(vq_sink);
    }
    if (aq_sink) {
        gst_object_unref(aq_sink);
    }

    /* Release tee request pads first so branch queues stop receiving data. */
    if (branch->video_tee_pad && mgr->video_tee) {
        LOG_I("unlink branch '%s': release video tee pad", branch->output_id);
        gst_element_release_request_pad(mgr->video_tee, branch->video_tee_pad);
        gst_object_unref(branch->video_tee_pad);
        branch->video_tee_pad = NULL;
    }
    if (branch->audio_tee_pad && mgr->audio_tee) {
        LOG_I("unlink branch '%s': release audio tee pad", branch->output_id);
        gst_element_release_request_pad(mgr->audio_tee, branch->audio_tee_pad);
        gst_object_unref(branch->audio_tee_pad);
        branch->audio_tee_pad = NULL;
    }

    if (branch->video_pacer && branch->video_gate_probe_id != 0) {
        GstPad *gate_pad = gst_element_get_static_pad(branch->video_pacer, "sink");
        if (gate_pad) {
            gst_pad_remove_probe(gate_pad, branch->video_gate_probe_id);
            gst_object_unref(gate_pad);
        }
        branch->video_gate_probe_id = 0;
    }
    if (branch->audio_queue && branch->audio_gate_probe_id != 0) {
        GstPad *gate_pad = gst_element_get_static_pad(branch->audio_queue, "sink");
        if (gate_pad) {
            gst_pad_remove_probe(gate_pad, branch->audio_gate_probe_id);
            gst_object_unref(gate_pad);
        }
        branch->audio_gate_probe_id = 0;
    }

    /* Stop the detached branch so sockets/files are released before reuse. */
    if (branch->sink) {
        /* For filesink, send EOS and drain */
        if (branch->sink_type && strcmp(branch->sink_type, "file") == 0) {
            gst_element_send_event(branch->video_queue, gst_event_new_eos());
            if (branch->audio_queue)
                gst_element_send_event(branch->audio_queue, gst_event_new_eos());
            /* Brief wait for EOS propagation */
            g_usleep(500000); /* 500ms */
        }
        LOG_I("unlink branch '%s': set sink NULL", branch->output_id);
        gst_element_set_state(branch->sink, GST_STATE_NULL);
        gst_element_get_state(branch->sink, NULL, NULL, 2 * GST_SECOND);
    }
    if (branch->muxer) {
        LOG_I("unlink branch '%s': set muxer NULL", branch->output_id);
        gst_element_set_state(branch->muxer, GST_STATE_NULL);
        gst_element_get_state(branch->muxer, NULL, NULL, GST_SECOND / 2);
    }
    if (branch->audio_queue) {
        LOG_I("unlink branch '%s': set audio queue NULL", branch->output_id);
        gst_element_set_state(branch->audio_queue, GST_STATE_NULL);
    }
    if (branch->video_pacer) {
        LOG_I("unlink branch '%s': set video pacer NULL", branch->output_id);
        gst_element_set_state(branch->video_pacer, GST_STATE_NULL);
    }
    if (branch->video_queue) {
        LOG_I("unlink branch '%s': set video queue NULL", branch->output_id);
        gst_element_set_state(branch->video_queue, GST_STATE_NULL);
    }

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        /* Give SRT a brief chance to release the listener socket without blocking RPCs. */
        g_usleep(50000);

        /* srtsink can finalize asynchronously after branch removal and crash if we
           drop the last reference while it is still winding down. Leave detached
           SRT branch elements owned by the pipeline and only remove them during
           full pipeline teardown. */
        LOG_I("unlink branch '%s': keeping detached SRT elements in pipeline", branch->output_id);
        branch->video_queue = NULL;
        branch->video_pacer = NULL;
        branch->audio_queue = NULL;
        branch->muxer = NULL;
        branch->sink = NULL;
        return;
    }

    /* Remove from pipeline bin */
    LOG_I("unlink branch '%s': remove branch elements from pipeline", branch->output_id);
    if (branch->sink) gst_bin_remove(GST_BIN(mgr->pipeline), branch->sink);
    if (branch->muxer) gst_bin_remove(GST_BIN(mgr->pipeline), branch->muxer);
    if (branch->audio_queue) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_queue);
    if (branch->video_pacer) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_pacer);
    if (branch->video_queue) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_queue);
    LOG_I("unlink branch '%s': remove complete", branch->output_id);

    branch->video_queue = NULL;
    branch->video_pacer = NULL;
    branch->audio_queue = NULL;
    branch->muxer       = NULL;
    branch->sink        = NULL;
}

/* ── Frame Reading (same as output_worker.c / preview.c) ──────── */

/* read_frame_to_buffer — REMOVED (was memfd mmap+memcpy path) */

/* ── Public API ───────────────────────────────────────────────── */

sbs_encoder_manager_t *sbs_encoder_manager_new(uint32_t width,
                                                uint32_t height,
                                                uint32_t fps_num,
                                                uint32_t fps_den,
                                                const sbs_encoder_config_t *enc_config)
{
    sbs_encoder_manager_t *mgr = g_new0(sbs_encoder_manager_t, 1);

    mgr->width   = width;
    mgr->height  = height;
    mgr->fps_num = fps_num;
    mgr->fps_den = fps_den > 0 ? fps_den : 1;

    mgr->codec            = g_strdup(enc_config->codec ? enc_config->codec : "h265");
    mgr->bitrate_kbps     = enc_config->bitrate_kbps > 0 ? enc_config->bitrate_kbps : 20000;
    mgr->gop_size         = enc_config->gop_size;
    mgr->gop_pattern      = enc_config->gop_pattern;
    mgr->rc_mode          = enc_config->rc_mode;
    mgr->encoder_override = g_strdup(enc_config->encoder);
    mgr->hdr10            = enc_config->hdr10;

    mgr->branches = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, sink_branch_free);
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mgr->pipeline_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    LOG_I("encoder manager created: %ux%u@%u/%u codec=%s bitrate=%u",
          width, height, fps_num, mgr->fps_den,
          mgr->codec, mgr->bitrate_kbps);

    return mgr;
}

void sbs_encoder_manager_free(sbs_encoder_manager_t *mgr)
{
    if (!mgr) return;

    /* Unlink all branches first */
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, mgr->branches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        unlink_sink_branch(mgr, value);
    }
    g_hash_table_destroy(mgr->branches);

    teardown_pipeline(mgr);

    g_free(mgr->codec);
    g_free(mgr->encoder_override);
    g_free(mgr->parser_name);
    pthread_mutex_destroy(&mgr->pipeline_mutex);
    g_free(mgr);
}

int sbs_encoder_manager_update_config(sbs_encoder_manager_t *mgr,
                                       const sbs_encoder_config_t *enc_config)
{
    if (!mgr || !enc_config) return SBS_ERR_INVAL;

    pthread_mutex_lock(&mgr->pipeline_mutex);

    /* Check if anything actually changed */
    const char *new_codec = enc_config->codec ? enc_config->codec : "h265";
    uint32_t new_bitrate = enc_config->bitrate_kbps > 0 ? enc_config->bitrate_kbps : 20000;

    bool codec_changed = (strcmp(mgr->codec, new_codec) != 0);
    bool bitrate_changed = (mgr->bitrate_kbps != new_bitrate);
    bool gop_changed = (mgr->gop_size != enc_config->gop_size);
    bool gop_pattern_changed = (mgr->gop_pattern != enc_config->gop_pattern);
    bool rc_mode_changed = (mgr->rc_mode != enc_config->rc_mode);
    bool hdr_changed = (mgr->hdr10 != enc_config->hdr10);

    if (!codec_changed && !bitrate_changed && !gop_changed && !gop_pattern_changed && !rc_mode_changed && !hdr_changed) {
        LOG_D("encoder config unchanged, skipping rebuild");
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return SBS_OK;
    }

    LOG_I("encoder config changed: codec=%s→%s bitrate=%u→%u gop=%u→%u gop_pattern=%d→%d rc_mode=%d→%d hdr10=%d→%d",
          mgr->codec, new_codec, mgr->bitrate_kbps, new_bitrate,
          mgr->gop_size, enc_config->gop_size, mgr->gop_pattern, enc_config->gop_pattern,
          mgr->rc_mode, enc_config->rc_mode, mgr->hdr10, enc_config->hdr10);

    mgr->pipeline_active = false;

    /* No need for sleep now — mutex guarantees no in-flight submit */

    /* Collect existing branch configs before teardown */
    GPtrArray *saved_configs = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, mgr->branches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sink_branch_t *branch = value;
        sink_branch_t *copy = g_new0(sink_branch_t, 1);
        copy->output_id    = g_strdup(branch->output_id);
        copy->sink_type    = g_strdup(branch->sink_type);
        copy->srt_uri      = g_strdup(branch->srt_uri);
        copy->srt_latency_ms = branch->srt_latency_ms;
        copy->rtmp_uri     = g_strdup(branch->rtmp_uri);
        copy->rtmp_passcode = g_strdup(branch->rtmp_passcode);
        copy->file_path    = g_strdup(branch->file_path);
        g_ptr_array_add(saved_configs, copy);

        /* Unlink before teardown */
        unlink_sink_branch(mgr, branch);
    }
    g_hash_table_remove_all(mgr->branches);

    /* Update config */
    g_free(mgr->codec);
    mgr->codec = g_strdup(new_codec);
    mgr->bitrate_kbps = new_bitrate;
    mgr->gop_size = enc_config->gop_size;
    mgr->gop_pattern = enc_config->gop_pattern;
    mgr->rc_mode = enc_config->rc_mode;
    g_free(mgr->encoder_override);
    mgr->encoder_override = g_strdup(enc_config->encoder);
    mgr->hdr10 = enc_config->hdr10;

    /* Teardown and rebuild */
    LOG_I("encoder teardown starting...");
    teardown_pipeline(mgr);
    LOG_I("encoder teardown done, rebuilding with hdr10=%d...", enc_config->hdr10);

    int rc = ensure_pipeline(mgr);
    LOG_I("encoder rebuild rc=%d", rc);
    if (rc != SBS_OK) {
        LOG_E("failed to rebuild encoder pipeline after config change");
        /* Free saved configs */
        for (guint i = 0; i < saved_configs->len; i++) {
            sink_branch_free(g_ptr_array_index(saved_configs, i));
        }
        g_ptr_array_free(saved_configs, TRUE);
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return rc;
    }

    /* Re-add all branches */
    for (guint i = 0; i < saved_configs->len; i++) {
        sink_branch_t *saved = g_ptr_array_index(saved_configs, i);
        sbs_sink_branch_config_t cfg = {
            .output_id    = saved->output_id,
            .sink_type    = saved->sink_type,
            .srt_uri      = saved->srt_uri,
            .srt_latency_ms = saved->srt_latency_ms,
            .rtmp_uri     = saved->rtmp_uri,
            .rtmp_passcode = saved->rtmp_passcode,
            .file_path    = saved->file_path,
        };
        int add_rc = sbs_encoder_manager_add_sink(mgr, &cfg);
        if (add_rc != SBS_OK) {
            LOG_W("failed to re-add branch '%s' after config change: %d",
                  saved->output_id, add_rc);
        }
    }

    /* Free saved configs */
    for (guint i = 0; i < saved_configs->len; i++) {
        sink_branch_free(g_ptr_array_index(saved_configs, i));
    }
    g_ptr_array_free(saved_configs, TRUE);

    LOG_I("encoder pipeline rebuilt successfully with %u branches",
          g_hash_table_size(mgr->branches));
    pthread_mutex_unlock(&mgr->pipeline_mutex);
    return SBS_OK;
}

/* sbs_encoder_manager_consume_frame — REMOVED (was fd-based memfd path) */

void sbs_encoder_manager_consume_frame_ptr(sbs_encoder_manager_t *mgr,
                                              const sbs_video_frame_msg_t *msg,
                                              const void *data,
                                              size_t size)
{
    if (!mgr || !msg || !data || size == 0)
        return;

    pthread_mutex_lock(&mgr->pipeline_mutex);

    if (!mgr->pipeline_active) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }

    sbs_direct_venc_packet_t packet = {0};
    int rc = sbs_direct_venc_submit_ptr(mgr->video_encoder, msg, data, size,
                                        mgr->force_next_idr, &packet);
    mgr->force_next_idr = false;
    if (rc != SBS_OK) {
        mgr->frames_dropped++;
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }

    push_encoded_packet(mgr, &packet);
    mgr->encoded_bytes += packet.size;

    mgr->frames_pushed++;
    if (mgr->frames_pushed <= 3 || mgr->frames_pushed % 600 == 0) {
        LOG_I("encoder frame encoded (ptr) #%lu (%ux%u, %u branches)",
              (unsigned long)mgr->frames_pushed,
              msg->width, msg->height,
              g_hash_table_size(mgr->branches));
    }
    pthread_mutex_unlock(&mgr->pipeline_mutex);
}

void sbs_encoder_manager_consume_frame_dmabuf(sbs_encoder_manager_t *mgr,
                                              const sbs_video_frame_msg_t *msg,
                                              int dmabuf_fd,
                                              size_t size)
{
    if (!mgr || !msg || dmabuf_fd < 0 || size == 0)
        return;

    pthread_mutex_lock(&mgr->pipeline_mutex);

    if (!mgr->pipeline_active) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        close(dmabuf_fd);
        return;
    }

    sbs_direct_venc_packet_t packet = {0};
    int rc = sbs_direct_venc_submit_dmabuf(mgr->video_encoder, msg, dmabuf_fd, size,
                                           mgr->force_next_idr, &packet);
    close(dmabuf_fd);
    mgr->force_next_idr = false;
    if (rc != SBS_OK) {
        mgr->frames_dropped++;
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }

    push_encoded_packet(mgr, &packet);
    mgr->encoded_bytes += packet.size;

    mgr->frames_pushed++;
    pthread_mutex_unlock(&mgr->pipeline_mutex);
}

void sbs_encoder_manager_consume_audio(sbs_encoder_manager_t *mgr,
                                        const sbs_audio_buffer_msg_t *msg,
                                        const void *audio_data)
{
    if (!mgr || !msg || !audio_data) return;

    pthread_mutex_lock(&mgr->pipeline_mutex);
    if (!mgr->pipeline_active || !mgr->audio_appsrc) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, msg->data_size, NULL);
    if (!buffer) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }
    gst_buffer_fill(buffer, 0, audio_data, msg->data_size);
    GST_BUFFER_PTS(buffer) = normalize_audio_time(mgr, msg->pts_ns);
    GST_BUFFER_DURATION(buffer) = msg->duration_ns == UINT64_MAX
        ? GST_CLOCK_TIME_NONE
        : msg->duration_ns;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(mgr->audio_appsrc), buffer);
    if (ret == GST_FLOW_OK) {
        mgr->audio_buffers_pushed++;
        if (mgr->audio_buffers_pushed <= 3 || mgr->audio_buffers_pushed % 600 == 0) {
            LOG_I("encoder audio pushed #%lu samples=%u size=%u",
                  (unsigned long)mgr->audio_buffers_pushed,
                  msg->n_samples,
                  msg->data_size);
        }
    } else if (mgr->audio_buffers_pushed % 100 == 0) {
        LOG_W("encoded audio appsrc push failed: %s", gst_flow_get_name(ret));
    }
    pthread_mutex_unlock(&mgr->pipeline_mutex);
}

int sbs_encoder_manager_add_sink(sbs_encoder_manager_t *mgr,
                                  const sbs_sink_branch_config_t *config)
{
    if (!mgr || !config || !config->output_id) return SBS_ERR_INVAL;

    pthread_mutex_lock(&mgr->pipeline_mutex);

    /* Check for duplicate */
    if (g_hash_table_contains(mgr->branches, config->output_id)) {
        LOG_W("sink branch '%s' already exists", config->output_id);
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return SBS_ERR_INVAL;
    }

    /* Ensure pipeline is running */
    int rc = ensure_pipeline(mgr);
    if (rc != SBS_OK) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return rc;
    }

    /* Create branch */
    sink_branch_t *branch = g_new0(sink_branch_t, 1);
    branch->output_id    = g_strdup(config->output_id);
    branch->sink_type    = g_strdup(config->sink_type ? config->sink_type : "srt");
    branch->srt_uri      = g_strdup(config->srt_uri);
    branch->srt_latency_ms = config->srt_latency_ms;
    branch->rtmp_uri     = g_strdup(config->rtmp_uri);
    branch->rtmp_passcode = g_strdup(config->rtmp_passcode);
    branch->file_path    = g_strdup(config->file_path);

    rc = link_sink_branch(mgr, branch);
    if (rc != SBS_OK) {
        sink_branch_free(branch);
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return rc;
    }

    g_hash_table_insert(mgr->branches, branch->output_id, branch);

    /* The next submitted frame becomes an IDR so late joiners start cleanly. */
    mgr->force_next_idr = true;
    LOG_I("scheduled IDR for new sink branch '%s'", config->output_id);

    LOG_I("sink branch '%s' added (%u total branches)",
          config->output_id, g_hash_table_size(mgr->branches));
    pthread_mutex_unlock(&mgr->pipeline_mutex);
    return SBS_OK;
}

int sbs_encoder_manager_remove_sink(sbs_encoder_manager_t *mgr,
                                     const char *output_id)
{
    if (!mgr || !output_id) return SBS_ERR_INVAL;

    pthread_mutex_lock(&mgr->pipeline_mutex);

    sink_branch_t *branch = g_hash_table_lookup(mgr->branches, output_id);
    if (!branch) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return SBS_ERR_NOT_FOUND;
    }

    g_hash_table_steal(mgr->branches, output_id);
    unlink_sink_branch(mgr, branch);
    sink_branch_free(branch);

    LOG_I("sink branch '%s' removed (%u remaining)",
          output_id, g_hash_table_size(mgr->branches));

    if (g_hash_table_size(mgr->branches) == 0) {
        LOG_I("last sink branch removed; tearing down encoder pipeline");
        teardown_pipeline(mgr);
    }

    pthread_mutex_unlock(&mgr->pipeline_mutex);
    return SBS_OK;
}

bool sbs_encoder_manager_has_sink(const sbs_encoder_manager_t *mgr,
                                   const char *output_id)
{
    if (!mgr || !output_id) return false;
    return g_hash_table_contains(mgr->branches, output_id);
}

uint32_t sbs_encoder_manager_sink_count(const sbs_encoder_manager_t *mgr)
{
    if (!mgr) return 0;
    return (uint32_t)g_hash_table_size(mgr->branches);
}

void sbs_encoder_manager_get_metrics(const sbs_encoder_manager_t *mgr,
                                      sbs_encoder_manager_metrics_t *metrics)
{
    sbs_encoder_manager_t *mutable_mgr;

    if (!metrics) return;
    memset(metrics, 0, sizeof(*metrics));
    if (!mgr) return;

    mutable_mgr = (sbs_encoder_manager_t *)mgr;
    pthread_mutex_lock(&mutable_mgr->pipeline_mutex);
    metrics->frames_pushed   = mutable_mgr->frames_pushed;
    metrics->frames_dropped  = mutable_mgr->frames_dropped;
    metrics->encoded_bytes   = mutable_mgr->encoded_bytes;
    metrics->active_branches = (uint32_t)g_hash_table_size(mutable_mgr->branches);
    metrics->pipeline_active = mutable_mgr->pipeline_active;
    pthread_mutex_unlock(&mutable_mgr->pipeline_mutex);
}

bool sbs_encoder_manager_is_active(const sbs_encoder_manager_t *mgr)
{
    return mgr && mgr->pipeline_active;
}

void sbs_encoder_manager_get_config(const sbs_encoder_manager_t *mgr,
                                     sbs_encoder_config_t *out_config)
{
    if (!mgr || !out_config) return;
    out_config->codec        = mgr->codec;
    out_config->bitrate_kbps = mgr->bitrate_kbps;
    out_config->gop_size     = mgr->gop_size;
    out_config->gop_pattern  = mgr->gop_pattern;
    out_config->rc_mode      = mgr->rc_mode;
    out_config->encoder      = mgr->encoder_override;
    out_config->hdr10        = mgr->hdr10;
}

void sbs_encoder_manager_get_format(const sbs_encoder_manager_t *mgr,
                                     uint32_t *width, uint32_t *height,
                                     uint32_t *fps_num, uint32_t *fps_den)
{
    if (!mgr) return;
    if (width)   *width   = mgr->width;
    if (height)  *height  = mgr->height;
    if (fps_num) *fps_num = mgr->fps_num;
    if (fps_den) *fps_den = mgr->fps_den;
}
