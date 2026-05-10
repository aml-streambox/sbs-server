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
    GstElement *audio_appsrc;
    GstElement *audio_convert;
    GstElement *audio_resample;
    GstElement *audio_encoder;
    GstElement *audio_parser;
    GstElement *audio_capsfilter;
    GstElement *muxer;
    GstElement *sink;
    GstElement *srt_pipeline;
    GstElement *srt_video_appsrc;
    GstElement *srt_audio_appsrc;
    bool        direct_audio;
    bool        direct_audio_base_valid;
    bool        direct_audio_next_valid;
    uint64_t    direct_audio_base_pts_ns;
    uint64_t    direct_audio_next_pts_ns;
    uint64_t    direct_audio_buffers_pushed;
    uint64_t    direct_audio_push_failures;

    /* Tee pad references */
    GstPad     *video_tee_pad;   /* request pad on video tee */
    GstPad     *audio_tee_pad;   /* request pad on audio tee */
    gulong      video_gate_probe_id;
    gulong      audio_gate_probe_id;
    gint        drop_until_keyframe;
    gint        srt_caller_count;
    GHashTable *srt_callers;
    bool        srt_video_base_valid;
    uint64_t    srt_video_base_pts_ns;
    uint64_t    srt_video_running_origin_ns;
    uint64_t    srt_video_buffers_pushed;
    uint64_t    srt_video_push_failures;
    bool        srt_stats_bytes_valid;
    uint64_t    srt_last_bytes_sent_total;
    uint64_t    srt_last_bytes_change_video_count;

    /* Deep-copied config for potential restart */
    char       *sink_type;
    char       *srt_uri;
    uint32_t    srt_latency_ms;
    char       *rtmp_uri;
    char       *rtmp_passcode;
    char       *file_path;
} sink_branch_t;

#define SBS_ENCODER_PACER_DELAY_NS (100ULL * GST_MSECOND)

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
    GPtrArray  *direct_audio_branches;

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
    bool     audio_next_pts_valid;
    uint64_t audio_next_pts_ns;
    uint64_t audio_last_capture_pts_ns;
    uint64_t audio_drift_warnings;
    uint64_t audio_push_failures;

    bool     pipeline_active;
    pthread_mutex_t pipeline_mutex;
    pthread_mutex_t audio_mutex;
};

/* ── Forward Declarations ─────────────────────────────────────── */

static int  ensure_pipeline(sbs_encoder_manager_t *mgr);
static void teardown_pipeline(sbs_encoder_manager_t *mgr);
static void sink_branch_free(gpointer data);
static int  link_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch);
static void unlink_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch);
static int  start_srt_session(sbs_encoder_manager_t *mgr, sink_branch_t *branch);
static void stop_srt_session(sbs_encoder_manager_t *mgr, sink_branch_t *branch);
static void flush_srt_session(sink_branch_t *branch);

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

    if (branch->srt_callers) {
        g_hash_table_add(branch->srt_callers, GINT_TO_POINTER(caller_id));
        g_atomic_int_set(&branch->srt_caller_count,
                         (gint)g_hash_table_size(branch->srt_callers));
    } else {
        g_atomic_int_set(&branch->srt_caller_count, 1);
    }
    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);

    pthread_mutex_lock(&mgr->pipeline_mutex);
    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
    mgr->force_next_idr = true;
    pthread_mutex_unlock(&mgr->pipeline_mutex);

    LOG_I("SRT caller %d connected; callers=%d scheduled IDR",
          caller_id, g_atomic_int_get(&branch->srt_caller_count));
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

    if (branch->srt_callers) {
        g_hash_table_remove(branch->srt_callers, GINT_TO_POINTER(caller_id));
        g_atomic_int_set(&branch->srt_caller_count,
                         (gint)g_hash_table_size(branch->srt_callers));
    } else {
        g_atomic_int_set(&branch->srt_caller_count, 0);
    }
    gint new_count = g_atomic_int_get(&branch->srt_caller_count);
    if (new_count == 0) {
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        flush_srt_session(branch);
    }

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
    if (g_atomic_int_get(&branch->srt_caller_count) <= 0)
        flush_srt_session(branch);

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

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;

    if (!g_atomic_int_get(&branch->drop_until_keyframe))
        return GST_PAD_PROBE_OK;

    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
        return GST_PAD_PROBE_DROP;

    if (branch->direct_audio) {
        sbs_encoder_manager_t *mgr = branch->manager;
        GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            if (mgr) pthread_mutex_lock(&mgr->audio_mutex);
            branch->direct_audio_base_valid = true;
            branch->direct_audio_next_valid = false;
            branch->direct_audio_base_pts_ns = pts;
            branch->direct_audio_next_pts_ns = 0;
            branch->direct_audio_buffers_pushed = 0;
            if (mgr) pthread_mutex_unlock(&mgr->audio_mutex);
        }
    }

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

static gboolean on_srt_session_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void)bus;

    sink_branch_t *branch = user_data;
    const char *output_id = branch && branch->output_id ? branch->output_id : "<unknown>";

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        LOG_E("SRT session '%s' error from %s: %s (%s)",
              output_id, GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        LOG_W("SRT session '%s' warning from %s: %s (%s)",
              output_id, GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_EOS:
        LOG_W("SRT session '%s' EOS", output_id);
        break;
    default:
        break;
    }

    return TRUE;
}

static void flush_appsrc_downstream(GstElement *appsrc)
{
    GstPad *src_pad;

    if (!appsrc)
        return;

    src_pad = gst_element_get_static_pad(appsrc, "src");
    if (!src_pad)
        return;

    gst_pad_push_event(src_pad, gst_event_new_flush_start());
    gst_pad_push_event(src_pad, gst_event_new_flush_stop(FALSE));
    gst_object_unref(src_pad);
}

static void flush_srt_session(sink_branch_t *branch)
{
    sbs_encoder_manager_t *mgr = branch ? branch->manager : NULL;

    if (!branch || !branch->srt_pipeline)
        return;

    branch->srt_video_base_valid = false;
    branch->srt_video_base_pts_ns = 0;
    branch->srt_video_running_origin_ns = 0;
    branch->srt_stats_bytes_valid = false;
    branch->srt_last_bytes_sent_total = 0;
    branch->srt_last_bytes_change_video_count = 0;
    if (mgr) pthread_mutex_lock(&mgr->audio_mutex);
    branch->direct_audio_base_valid = false;
    branch->direct_audio_next_valid = false;
    branch->direct_audio_base_pts_ns = 0;
    branch->direct_audio_next_pts_ns = 0;
    branch->direct_audio_buffers_pushed = 0;
    if (mgr) pthread_mutex_unlock(&mgr->audio_mutex);

    flush_appsrc_downstream(branch->srt_video_appsrc);
    flush_appsrc_downstream(branch->srt_audio_appsrc);
    LOG_I("SRT session '%s' flushed", branch->output_id);
}

static bool update_srt_sink_stats(sink_branch_t *branch)
{
    GstStructure *stats = NULL;
    guint64 bytes_sent_total = 0;
    uint64_t stale_frames;
    gboolean have_bytes;

    if (!branch || !branch->sink || g_atomic_int_get(&branch->srt_caller_count) <= 0)
        return false;

    g_object_get(branch->sink, "stats", &stats, NULL);
    if (!stats)
        return true;

    have_bytes = gst_structure_get_uint64(stats, "bytes-sent-total", &bytes_sent_total);
    gst_structure_free(stats);
    if (!have_bytes)
        return true;

    if (!branch->srt_stats_bytes_valid) {
        branch->srt_stats_bytes_valid = true;
        branch->srt_last_bytes_sent_total = bytes_sent_total;
        branch->srt_last_bytes_change_video_count = branch->srt_video_buffers_pushed;
        return true;
    }

    if (bytes_sent_total > branch->srt_last_bytes_sent_total) {
        branch->srt_last_bytes_sent_total = bytes_sent_total;
        branch->srt_last_bytes_change_video_count = branch->srt_video_buffers_pushed;
        return true;
    }

    stale_frames = branch->srt_video_buffers_pushed > branch->srt_last_bytes_change_video_count
        ? branch->srt_video_buffers_pushed - branch->srt_last_bytes_change_video_count
        : 0;
    if (stale_frames > 180) {
        LOG_W("SRT session '%s' caller stale: no bytes sent for %lu video frames; expiring caller gate",
              branch->output_id ? branch->output_id : "<unknown>",
              (unsigned long)stale_frames);
        if (branch->srt_callers)
            g_hash_table_remove_all(branch->srt_callers);
        g_atomic_int_set(&branch->srt_caller_count, 0);
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        flush_srt_session(branch);
        return false;
    }

    return true;
}

/* ── Pipeline Construction ────────────────────────────────────── */

static GstClockTime element_running_time(GstElement *pipeline)
{
    GstClockTime now;
    GstClockTime base;
    GstClock *clock;

    if (!pipeline)
        return 0;

    clock = gst_element_get_clock(pipeline);
    if (!clock)
        return 0;

    now = gst_clock_get_time(clock);
    base = gst_element_get_base_time(pipeline);
    gst_object_unref(clock);

    if (!GST_CLOCK_TIME_IS_VALID(now) || !GST_CLOCK_TIME_IS_VALID(base) || now < base)
        return 0;

    return now - base;
}

static GstClockTime pipeline_running_time(sbs_encoder_manager_t *mgr)
{
    return mgr ? element_running_time(mgr->pipeline) : 0;
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

static GstClockTime next_audio_sample_time(sbs_encoder_manager_t *mgr,
                                           const sbs_audio_buffer_msg_t *msg,
                                           uint64_t duration_ns)
{
    GstClockTime pts;

    if (!mgr->audio_next_pts_valid) {
        pts = normalize_audio_time(mgr, msg->pts_ns);
        mgr->audio_next_pts_ns = pts;
        mgr->audio_next_pts_valid = true;
        mgr->audio_last_capture_pts_ns = msg->pts_ns;
    } else {
        pts = (GstClockTime)mgr->audio_next_pts_ns;
        if (msg->pts_ns != UINT64_MAX && mgr->audio_last_capture_pts_ns != UINT64_MAX) {
            uint64_t expected = mgr->audio_last_capture_pts_ns + duration_ns;
            uint64_t diff = msg->pts_ns > expected ? msg->pts_ns - expected : expected - msg->pts_ns;
            if (diff > 5 * GST_MSECOND) {
                mgr->audio_drift_warnings++;
                if (mgr->audio_drift_warnings <= 5 || mgr->audio_drift_warnings % 100 == 0) {
                    LOG_W("audio capture timestamp drift #%lu diff=%luns queue_pts=%luns capture_pts=%luns",
                          (unsigned long)mgr->audio_drift_warnings,
                          (unsigned long)diff,
                          (unsigned long)pts,
                          (unsigned long)msg->pts_ns);
                }
            }
            mgr->audio_last_capture_pts_ns = msg->pts_ns;
        }
    }

    mgr->audio_next_pts_ns += duration_ns;
    return pts;
}

static GstBuffer *make_audio_buffer(const sbs_audio_buffer_msg_t *msg,
                                    const void *audio_data,
                                    GstClockTime pts_ns,
                                    uint64_t duration_ns)
{
    GstBuffer *buffer;

    if (!msg || !audio_data || msg->data_size == 0)
        return NULL;

    buffer = gst_buffer_new_allocate(NULL, msg->data_size, NULL);
    if (!buffer)
        return NULL;

    gst_buffer_fill(buffer, 0, audio_data, msg->data_size);
    GST_BUFFER_PTS(buffer) = pts_ns;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = duration_ns;
    return buffer;
}

static void push_srt_session_packet(sbs_encoder_manager_t *mgr,
                                    sink_branch_t *branch,
                                    const sbs_direct_venc_packet_t *packet,
                                    GstClockTime pts_ns,
                                    GstClockTime dts_ns)
{
    GstClockTime rel_pts;
    GstClockTime rel_dts;
    GstBuffer *buffer;
    GstFlowReturn ret;

    if (!mgr || !branch || !branch->srt_video_appsrc || !packet ||
        !packet->data || packet->size == 0)
        return;

    if (g_atomic_int_get(&branch->srt_caller_count) <= 0) {
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        return;
    }

    if (g_atomic_int_get(&branch->drop_until_keyframe)) {
        if (!packet->is_keyframe)
            return;

        GstClockTime running_origin = element_running_time(branch->srt_pipeline) +
                                      SBS_ENCODER_PACER_DELAY_NS;
        branch->srt_video_base_valid = true;
        branch->srt_video_base_pts_ns = pts_ns;
        branch->srt_video_running_origin_ns = running_origin;
        branch->srt_video_buffers_pushed = 0;
        branch->srt_stats_bytes_valid = false;
        branch->srt_last_bytes_sent_total = 0;
        branch->srt_last_bytes_change_video_count = 0;
        pthread_mutex_lock(&mgr->audio_mutex);
        branch->direct_audio_base_valid = true;
        branch->direct_audio_next_valid = false;
        branch->direct_audio_base_pts_ns = running_origin;
        branch->direct_audio_next_pts_ns = 0;
        branch->direct_audio_buffers_pushed = 0;
        pthread_mutex_unlock(&mgr->audio_mutex);
        g_atomic_int_set(&branch->drop_until_keyframe, FALSE);
        LOG_I("SRT session '%s' resumed at keyframe", branch->output_id);
    }

    if (!branch->srt_video_base_valid)
        return;

    if (g_atomic_int_get(&branch->srt_caller_count) <= 0) {
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        return;
    }

    rel_pts = branch->srt_video_running_origin_ns +
        (pts_ns >= branch->srt_video_base_pts_ns
            ? pts_ns - branch->srt_video_base_pts_ns
            : 0);
    rel_dts = GST_CLOCK_TIME_IS_VALID(dts_ns)
        ? branch->srt_video_running_origin_ns +
            (dts_ns >= branch->srt_video_base_pts_ns
                ? dts_ns - branch->srt_video_base_pts_ns
                : 0)
        : GST_CLOCK_TIME_NONE;

    buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
    if (!buffer)
        return;

    gst_buffer_fill(buffer, 0, packet->data, packet->size);
    GST_BUFFER_PTS(buffer) = rel_pts;
    GST_BUFFER_DTS(buffer) = rel_dts;
    GST_BUFFER_DURATION(buffer) = packet->duration_ns;
    if (packet->is_keyframe)
        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    else
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    ret = gst_app_src_push_buffer(GST_APP_SRC(branch->srt_video_appsrc), buffer);
    if (ret == GST_FLOW_OK) {
        branch->srt_video_buffers_pushed++;
        if (branch->srt_video_buffers_pushed <= 3 || branch->srt_video_buffers_pushed % 120 == 0) {
            if (!update_srt_sink_stats(branch))
                return;
        }
        if (branch->srt_video_buffers_pushed <= 3 || branch->srt_video_buffers_pushed % 600 == 0) {
            LOG_I("SRT session '%s' video pushed #%lu pts=%luns size=%zu",
                  branch->output_id,
                  (unsigned long)branch->srt_video_buffers_pushed,
                  (unsigned long)rel_pts,
                  packet->size);
        }
    } else {
        branch->srt_video_push_failures++;
        if (branch->srt_video_push_failures <= 5 || branch->srt_video_push_failures % 100 == 0) {
            LOG_W("SRT session '%s' video appsrc push failed #%lu: %s",
                  branch->output_id,
                  (unsigned long)branch->srt_video_push_failures,
                  gst_flow_get_name(ret));
        }
    }
}

static void direct_audio_branch_remove(sbs_encoder_manager_t *mgr,
                                        sink_branch_t *branch)
{
    if (!mgr || !branch || !mgr->direct_audio_branches)
        return;

    for (guint i = 0; i < mgr->direct_audio_branches->len; i++) {
        if (g_ptr_array_index(mgr->direct_audio_branches, i) == branch) {
            g_ptr_array_remove_index_fast(mgr->direct_audio_branches, i);
            return;
        }
    }
}

static GstClockTime direct_audio_branch_next_pts(sink_branch_t *branch,
                                                 GstClockTime fallback_pts,
                                                 uint64_t duration_ns)
{
    (void)duration_ns;
    if (!branch || !branch->direct_audio_base_valid)
        return fallback_pts;

    if (!branch->direct_audio_next_valid) {
        branch->direct_audio_next_pts_ns = fallback_pts;
        branch->direct_audio_next_valid = true;
        return branch->direct_audio_base_pts_ns;
    }

    if (fallback_pts < branch->direct_audio_next_pts_ns)
        return branch->direct_audio_base_pts_ns;

    return branch->direct_audio_base_pts_ns +
           (fallback_pts - branch->direct_audio_next_pts_ns);
}

static void push_encoded_packet(sbs_encoder_manager_t *mgr,
                                const sbs_direct_venc_packet_t *packet)
{
    GstClockTime pts_ns;
    GstClockTime dts_ns;

    if (!mgr || !mgr->video_appsrc || !packet || !packet->data || packet->size == 0)
        return;

    dts_ns = (packet->dts_ns == UINT64_MAX)
        ? GST_CLOCK_TIME_NONE
        : normalize_video_time(mgr, packet->dts_ns);
    pts_ns = normalize_video_time(mgr, packet->pts_ns);

    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, mgr->branches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sink_branch_t *branch = value;
        if (branch->srt_pipeline)
            push_srt_session_packet(mgr, branch, packet, pts_ns, dts_ns);
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
    if (!buffer) {
        mgr->frames_dropped++;
        return;
    }

    gst_buffer_fill(buffer, 0, packet->data, packet->size);
    GST_BUFFER_DTS(buffer) = dts_ns;
    GST_BUFFER_PTS(buffer) = pts_ns;
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
        "max-bytes",    48000 * 2 * 2 / 4,
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
        "max-size-time",    (guint64)(250 * GST_MSECOND),
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
    pthread_mutex_lock(&mgr->audio_mutex);
    mgr->pipeline_active = true;
    mgr->audio_next_pts_valid = false;
    mgr->audio_next_pts_ns = 0;
    mgr->audio_last_capture_pts_ns = UINT64_MAX;
    mgr->audio_drift_warnings = 0;
    mgr->audio_push_failures = 0;
    pthread_mutex_unlock(&mgr->audio_mutex);

    LOG_I("encoder pipeline PLAYING: %ux%u@%u/%u codec=%s bitrate=%u",
          mgr->width, mgr->height, mgr->fps_num, mgr->fps_den,
          mgr->codec ? mgr->codec : "h265", mgr->bitrate_kbps);

    return SBS_OK;
}

static void teardown_pipeline(sbs_encoder_manager_t *mgr)
{
    if (!mgr->pipeline) return;

    LOG_I("tearing down encoder pipeline (pushed %lu frames)", (unsigned long)mgr->frames_pushed);
    pthread_mutex_lock(&mgr->audio_mutex);
    mgr->pipeline_active = false;
    pthread_mutex_unlock(&mgr->audio_mutex);

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
        pthread_mutex_lock(&mgr->audio_mutex);
        mgr->pipeline_active = false;
        gst_object_unref(mgr->audio_appsrc);
        mgr->audio_appsrc = NULL;
        mgr->audio_next_pts_valid = false;
        pthread_mutex_unlock(&mgr->audio_mutex);
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
    pthread_mutex_lock(&mgr->audio_mutex);
    mgr->pipeline_active = false;
    pthread_mutex_unlock(&mgr->audio_mutex);

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
    if (branch->srt_callers)
        g_hash_table_destroy(branch->srt_callers);

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
            sink = gst_element_factory_make("srtserversink", NULL);
            if (!sink)
                sink = gst_element_factory_make("srtsink", NULL);
            if (sink) {
                uint32_t latency_ms = config->srt_latency_ms > 0
                    ? config->srt_latency_ms : 600;
                g_object_set(sink,
                    "uri",                 config->srt_uri,
                    "mode",                (gint)2,
                    "wait-for-connection", FALSE,
                    "poll-timeout",        (gint)100,
                    "latency",             (gint)latency_ms,
                    "blocksize",           (guint)1316,
                    "sync",                FALSE,
                    "async",               FALSE,
                    NULL);
                LOG_I("SRT server sink: %s latency=%ums sync=0 blocksize=1316", config->srt_uri, latency_ms);
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

static int start_srt_session(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    const char *muxer_format = NULL;
    GstElement *pipeline = NULL;
    GstElement *video_src = NULL;
    GstElement *video_queue = NULL;
    GstElement *parser = NULL;
    GstElement *video_pacer = NULL;
    GstElement *audio_src = NULL;
    GstElement *audio_queue = NULL;
    GstElement *audio_convert = NULL;
    GstElement *audio_resample = NULL;
    GstElement *audio_encoder = NULL;
    GstElement *audio_parser = NULL;
    GstElement *audio_capsfilter = NULL;
    GstElement *muxer = NULL;
    GstElement *sink = NULL;
    GstCaps *video_caps = NULL;
    GstCaps *audio_caps = NULL;
    GstCaps *mp2_caps = NULL;

    if (!mgr || !branch)
        return SBS_ERR_INVAL;

    ensure_gstreamer_ready();

    pipeline = gst_pipeline_new(NULL);
    video_src = gst_element_factory_make("appsrc", NULL);
    video_queue = gst_element_factory_make("queue", NULL);
    parser = gst_element_factory_make(mgr->is_h265 ? "h265parse" : "h264parse", NULL);
    video_pacer = gst_element_factory_make("identity", NULL);
    audio_src = gst_element_factory_make("appsrc", NULL);
    audio_queue = gst_element_factory_make("queue", NULL);
    audio_convert = gst_element_factory_make("audioconvert", NULL);
    audio_resample = gst_element_factory_make("audioresample", NULL);
    audio_encoder = gst_element_factory_make("avenc_mp2", NULL);
    if (!audio_encoder)
        audio_encoder = gst_element_factory_make("avenc_mp2fixed", NULL);
    audio_parser = gst_element_factory_make("mpegaudioparse", NULL);
    audio_capsfilter = gst_element_factory_make("capsfilter", NULL);
    muxer = create_muxer(mgr->codec, "srt", &muxer_format);

    sbs_sink_branch_config_t cfg = {
        .output_id = branch->output_id,
        .sink_type = branch->sink_type,
        .srt_uri = branch->srt_uri,
        .srt_latency_ms = branch->srt_latency_ms,
    };
    sink = create_sink(&cfg, muxer_format);

    if (!pipeline || !video_src || !video_queue || !parser || !video_pacer || !audio_src ||
        !audio_queue || !audio_convert || !audio_resample || !audio_encoder ||
        !audio_parser || !audio_capsfilter || !muxer || !sink) {
        LOG_E("failed to create SRT session elements for '%s'", branch->output_id);
        if (pipeline) gst_object_unref(pipeline);
        if (video_src) gst_object_unref(video_src);
        if (video_queue) gst_object_unref(video_queue);
        if (parser) gst_object_unref(parser);
        if (video_pacer) gst_object_unref(video_pacer);
        if (audio_src) gst_object_unref(audio_src);
        if (audio_queue) gst_object_unref(audio_queue);
        if (audio_convert) gst_object_unref(audio_convert);
        if (audio_resample) gst_object_unref(audio_resample);
        if (audio_encoder) gst_object_unref(audio_encoder);
        if (audio_parser) gst_object_unref(audio_parser);
        if (audio_capsfilter) gst_object_unref(audio_capsfilter);
        if (muxer) gst_object_unref(muxer);
        if (sink) gst_object_unref(sink);
        return SBS_ERR_IO;
    }

    video_caps = gst_caps_new_simple(mgr->is_h265 ? "video/x-h265" : "video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment",     G_TYPE_STRING, "au",
        NULL);
    g_object_set(video_src,
        "caps",         video_caps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    (guint64)(mgr->bitrate_kbps * 1000 / 2),
        NULL);
    gst_caps_unref(video_caps);

    audio_caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "S16LE",
        "rate",     G_TYPE_INT, 48000,
        "channels", G_TYPE_INT, 2,
        "layout",   G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(audio_src,
        "caps",         audio_caps,
        "format",       GST_FORMAT_TIME,
        "is-live",      TRUE,
        "do-timestamp", FALSE,
        "block",        FALSE,
        "max-bytes",    48000 * 2 * 2 / 2,
        NULL);
    gst_caps_unref(audio_caps);

    mp2_caps = gst_caps_new_simple("audio/mpeg",
        "parsed",      G_TYPE_BOOLEAN, TRUE,
        "mpegversion", G_TYPE_INT, 1,
        "layer",       G_TYPE_INT, 2,
        NULL);
    g_object_set(audio_capsfilter, "caps", mp2_caps, NULL);
    gst_caps_unref(mp2_caps);

    g_object_set(video_queue,
        "max-size-buffers", (guint)8,
        "max-size-time",    (guint64)(250 * GST_MSECOND),
        "max-size-bytes",   (guint)0,
        "leaky",            2,
        NULL);
    g_object_set(audio_queue,
        "max-size-buffers", (guint)20,
        "max-size-time",    (guint64)(200 * GST_MSECOND),
        "max-size-bytes",   (guint)0,
        "leaky",            2,
        NULL);
    g_object_set(video_pacer,
        "sync", TRUE,
        NULL);
    g_object_set(audio_encoder, "bitrate", 192000, NULL);
    g_object_set(parser,
        "config-interval",    (gint)-1,
        "disable-passthrough", TRUE,
        NULL);
    g_signal_connect(sink, "caller-connecting",
                     G_CALLBACK(on_srt_caller_connecting), branch);
    g_signal_connect(sink, "caller-added",
                     G_CALLBACK(on_srt_caller_added), branch);
    g_signal_connect(sink, "caller-removed",
                     G_CALLBACK(on_srt_caller_removed), branch);

    gst_bin_add_many(GST_BIN(pipeline),
        video_src, video_queue, parser, video_pacer,
        audio_src, audio_queue, audio_convert, audio_resample, audio_encoder,
        audio_parser, audio_capsfilter,
        muxer, sink,
        NULL);

    if (!gst_element_link_many(video_src, video_queue, parser, video_pacer, muxer, sink, NULL) ||
        !gst_element_link_many(audio_src, audio_queue, audio_convert, audio_resample,
                               audio_encoder, audio_parser, audio_capsfilter, muxer, NULL)) {
        LOG_E("failed to link SRT session for '%s'", branch->output_id);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return SBS_ERR_IO;
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, on_srt_session_bus_message, branch);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_E("SRT session '%s' failed to enter PLAYING", branch->output_id);
        bus = gst_element_get_bus(pipeline);
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        return SBS_ERR_IO;
    }

    branch->manager = mgr;
    branch->srt_pipeline = pipeline;
    branch->srt_video_appsrc = gst_object_ref(video_src);
    branch->srt_audio_appsrc = gst_object_ref(audio_src);
    branch->audio_appsrc = branch->srt_audio_appsrc;
    branch->muxer = muxer;
    branch->sink = sink;
    branch->direct_audio = true;
    branch->drop_until_keyframe = TRUE;
    branch->srt_caller_count = 0;
    if (!branch->srt_callers)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);
    else
        g_hash_table_remove_all(branch->srt_callers);
    branch->srt_video_base_valid = false;
    branch->srt_video_base_pts_ns = 0;
    branch->srt_video_running_origin_ns = 0;
    branch->srt_video_buffers_pushed = 0;
    branch->srt_video_push_failures = 0;
    branch->srt_stats_bytes_valid = false;
    branch->srt_last_bytes_sent_total = 0;
    branch->srt_last_bytes_change_video_count = 0;

    LOG_I("SRT session '%s' started", branch->output_id);
    return SBS_OK;
}

static void stop_srt_session(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    GstElement *pipeline;
    GstElement *video_appsrc;
    GstElement *audio_appsrc;

    if (!branch || !branch->srt_pipeline)
        return;

    LOG_I("stopping SRT session '%s'", branch->output_id);

    pipeline = branch->srt_pipeline;
    video_appsrc = branch->srt_video_appsrc;

    pthread_mutex_lock(&mgr->audio_mutex);
    audio_appsrc = branch->srt_audio_appsrc;
    branch->audio_appsrc = NULL;
    branch->srt_audio_appsrc = NULL;
    branch->direct_audio = false;
    pthread_mutex_unlock(&mgr->audio_mutex);

    branch->srt_pipeline = NULL;
    branch->srt_video_appsrc = NULL;
    branch->muxer = NULL;
    branch->sink = NULL;

    if (video_appsrc)
        gst_app_src_end_of_stream(GST_APP_SRC(video_appsrc));
    if (audio_appsrc)
        gst_app_src_end_of_stream(GST_APP_SRC(audio_appsrc));

    gst_element_set_state(pipeline, GST_STATE_NULL);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);

    if (video_appsrc)
        gst_object_unref(video_appsrc);
    if (audio_appsrc)
        gst_object_unref(audio_appsrc);
    gst_object_unref(pipeline);
}

static int link_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    GstElement *vqueue = gst_element_factory_make("queue", NULL);
    GstElement *vpacer = gst_element_factory_make("identity", NULL);
    GstElement *aqueue = gst_element_factory_make("queue", NULL);
    const char *muxer_format = NULL;
    GstElement *muxer  = create_muxer(mgr->codec, branch->sink_type, &muxer_format);
    GstElement *acapsfilter = NULL;
    GstElement *branch_audio_src = NULL;
    GstElement *branch_audio_convert = NULL;
    GstElement *branch_audio_resample = NULL;
    GstElement *branch_audio_encoder = NULL;
    GstElement *branch_audio_parser = NULL;
    bool direct_audio = branch->sink_type && strcmp(branch->sink_type, "srt") == 0;

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
    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 && !branch->srt_callers)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);

    if (muxer_format && strcmp(muxer_format, "mpegts") == 0) {
        GstCaps *caps;
        acapsfilter = gst_element_factory_make("capsfilter", NULL);
        if (direct_audio) {
            caps = gst_caps_new_simple("audio/mpeg",
                "parsed",      G_TYPE_BOOLEAN, TRUE,
                "mpegversion", G_TYPE_INT, 1,
                "layer",       G_TYPE_INT, 2,
                NULL);
        } else {
            caps = gst_caps_new_simple("audio/mpeg",
                "framed",        G_TYPE_BOOLEAN, TRUE,
                "mpegversion",   G_TYPE_INT, 4,
                "stream-format", G_TYPE_STRING, "adts",
                NULL);
        }
        if (acapsfilter)
            g_object_set(acapsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
    }

    if (direct_audio) {
        branch_audio_src = gst_element_factory_make("appsrc", NULL);
        branch_audio_convert = gst_element_factory_make("audioconvert", NULL);
        branch_audio_resample = gst_element_factory_make("audioresample", NULL);
        branch_audio_encoder = gst_element_factory_make("avenc_mp2", NULL);
        if (!branch_audio_encoder)
            branch_audio_encoder = gst_element_factory_make("avenc_mp2fixed", NULL);
        branch_audio_parser = gst_element_factory_make("mpegaudioparse", NULL);
    }

    if (!vqueue || !vpacer || !aqueue || !muxer || !sink ||
        (muxer_format && strcmp(muxer_format, "mpegts") == 0 && !acapsfilter) ||
        (direct_audio && (!branch_audio_src || !branch_audio_convert ||
                          !branch_audio_resample || !branch_audio_encoder ||
                          !branch_audio_parser))) {
        LOG_E("failed to create sink branch elements for '%s'", branch->output_id);
        if (vqueue) gst_object_unref(vqueue);
        if (vpacer) gst_object_unref(vpacer);
        if (aqueue) gst_object_unref(aqueue);
        if (branch_audio_src) gst_object_unref(branch_audio_src);
        if (branch_audio_convert) gst_object_unref(branch_audio_convert);
        if (branch_audio_resample) gst_object_unref(branch_audio_resample);
        if (branch_audio_encoder) gst_object_unref(branch_audio_encoder);
        if (branch_audio_parser) gst_object_unref(branch_audio_parser);
        if (acapsfilter) gst_object_unref(acapsfilter);
        if (muxer) gst_object_unref(muxer);
        if (sink) gst_object_unref(sink);
        return SBS_ERR_IO;
    }

    if (direct_audio) {
        GstCaps *raw_caps = gst_caps_new_simple("audio/x-raw",
            "format",   G_TYPE_STRING, "S16LE",
            "rate",     G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "layout",   G_TYPE_STRING, "interleaved",
            NULL);
        g_object_set(branch_audio_src,
            "caps",         raw_caps,
            "format",       GST_FORMAT_TIME,
            "is-live",      TRUE,
            "do-timestamp", FALSE,
            "block",        FALSE,
            "max-bytes",    48000 * 2 * 2 / 2,
            NULL);
        gst_caps_unref(raw_caps);
        g_object_set(branch_audio_encoder, "bitrate", 192000, NULL);
    }

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        g_signal_connect(sink, "caller-connecting",
                         G_CALLBACK(on_srt_caller_connecting), branch);
        g_signal_connect(sink, "caller-added",
                         G_CALLBACK(on_srt_caller_added), branch);
        g_signal_connect(sink, "caller-removed",
                         G_CALLBACK(on_srt_caller_removed), branch);
    }

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        g_object_set(vqueue,
            "max-size-buffers", (guint)8,
            "max-size-time",    (guint64)(250 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    } else {
        g_object_set(vqueue,
            "max-size-buffers", (guint)8,
            "max-size-time",    (guint64)(250 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    }

    if (direct_audio) {
        g_object_set(aqueue,
            "max-size-buffers", (guint)20,
            "max-size-time",    (guint64)(200 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    } else {
        g_object_set(aqueue,
            "max-size-buffers", (guint)0,
            "max-size-time",    (guint64)(100 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    }

    g_object_set(vpacer,
        "sync", TRUE,
        NULL);

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0) {
        GstPad *gate_pad = gst_element_get_static_pad(vqueue, "sink");
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
    if (direct_audio) {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer,
                         branch_audio_src, aqueue, branch_audio_convert,
                         branch_audio_resample, branch_audio_encoder,
                         branch_audio_parser, acapsfilter, muxer, sink, NULL);
    } else if (acapsfilter) {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue,
                         acapsfilter, muxer, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue,
                         muxer, sink, NULL);
    }

    /* Link: vqueue → timestamp pacer → muxer → sink */
    if (!gst_element_link_many(vqueue, vpacer, muxer, sink, NULL)) {
        LOG_E("failed to link video branch for '%s'", branch->output_id);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }

    if (direct_audio) {
        if (!gst_element_link_many(branch_audio_src, aqueue, branch_audio_convert,
                                   branch_audio_resample, branch_audio_encoder,
                                   branch_audio_parser, acapsfilter, muxer, NULL)) {
            LOG_E("failed to link direct MP2 audio branch into muxer for '%s'", branch->output_id);
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer,
                                branch_audio_src, aqueue, branch_audio_convert,
                                branch_audio_resample, branch_audio_encoder,
                                branch_audio_parser, acapsfilter, muxer, sink, NULL);
            return SBS_ERR_IO;
        }
    } else if (acapsfilter) {
        if (!gst_element_link_many(aqueue, acapsfilter, muxer, NULL)) {
            LOG_E("failed to link ADTS audio branch into muxer for '%s'", branch->output_id);
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
            return SBS_ERR_IO;
        }
    } else if (!gst_element_link(aqueue, muxer)) {
        LOG_E("failed to link audio branch into muxer for '%s'", branch->output_id);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }

    /* Sync state with parent pipeline */
    gst_element_sync_state_with_parent(vqueue);
    gst_element_sync_state_with_parent(vpacer);
    if (branch_audio_src) gst_element_sync_state_with_parent(branch_audio_src);
    gst_element_sync_state_with_parent(aqueue);
    if (branch_audio_convert) gst_element_sync_state_with_parent(branch_audio_convert);
    if (branch_audio_resample) gst_element_sync_state_with_parent(branch_audio_resample);
    if (branch_audio_encoder) gst_element_sync_state_with_parent(branch_audio_encoder);
    if (branch_audio_parser) gst_element_sync_state_with_parent(branch_audio_parser);
    if (acapsfilter) gst_element_sync_state_with_parent(acapsfilter);
    gst_element_sync_state_with_parent(muxer);
    gst_element_sync_state_with_parent(sink);

    /* Request pad from video tee and link */
    GstPad *vtee_pad = gst_element_request_pad_simple(mgr->video_tee, "src_%u");
    GstPad *vq_sink  = gst_element_get_static_pad(vqueue, "sink");
    if (!vtee_pad || !vq_sink) {
        LOG_E("failed to request video branch pads for '%s'", branch->output_id);
        if (vtee_pad) gst_object_unref(vtee_pad);
        if (vq_sink) gst_object_unref(vq_sink);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    if (gst_pad_link(vtee_pad, vq_sink) != GST_PAD_LINK_OK) {
        LOG_E("failed to link video tee to queue for '%s'", branch->output_id);
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        gst_object_unref(vq_sink);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    gst_object_unref(vq_sink);

    GstPad *atee_pad = NULL;
    GstPad *aq_sink  = NULL;
    if (!direct_audio) {
        atee_pad = gst_element_request_pad_simple(mgr->audio_tee, "src_%u");
        aq_sink  = gst_element_get_static_pad(aqueue, "sink");
    }
    if (!direct_audio && (!atee_pad || !aq_sink)) {
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
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    if (!direct_audio && gst_pad_link(atee_pad, aq_sink) != GST_PAD_LINK_OK) {
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
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    gst_object_unref(aq_sink);

    /* Store references in branch. */
    branch->video_queue    = vqueue;
    branch->video_pacer    = vpacer;
    branch->audio_appsrc   = branch_audio_src;
    branch->audio_queue    = aqueue;
    branch->audio_convert  = branch_audio_convert;
    branch->audio_resample = branch_audio_resample;
    branch->audio_encoder  = branch_audio_encoder;
    branch->audio_parser   = branch_audio_parser;
    branch->audio_capsfilter = acapsfilter;
    branch->muxer          = muxer;
    branch->sink           = sink;
    branch->video_tee_pad  = vtee_pad;
    branch->audio_tee_pad  = atee_pad;
    branch->direct_audio   = direct_audio;

    LOG_I("sink branch '%s' linked (type=%s)", branch->output_id,
          branch->sink_type ? branch->sink_type : "srt");

    return SBS_OK;
}

static void unlink_sink_branch(sbs_encoder_manager_t *mgr, sink_branch_t *branch)
{
    if (branch && branch->srt_pipeline) {
        stop_srt_session(mgr, branch);
        return;
    }

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
    if (branch->audio_parser) {
        LOG_I("unlink branch '%s': set audio parser NULL", branch->output_id);
        gst_element_set_state(branch->audio_parser, GST_STATE_NULL);
    }
    if (branch->audio_encoder) {
        LOG_I("unlink branch '%s': set audio encoder NULL", branch->output_id);
        gst_element_set_state(branch->audio_encoder, GST_STATE_NULL);
    }
    if (branch->audio_resample) {
        LOG_I("unlink branch '%s': set audio resample NULL", branch->output_id);
        gst_element_set_state(branch->audio_resample, GST_STATE_NULL);
    }
    if (branch->audio_convert) {
        LOG_I("unlink branch '%s': set audio convert NULL", branch->output_id);
        gst_element_set_state(branch->audio_convert, GST_STATE_NULL);
    }
    if (branch->audio_appsrc) {
        LOG_I("unlink branch '%s': set direct audio appsrc NULL", branch->output_id);
        gst_element_set_state(branch->audio_appsrc, GST_STATE_NULL);
    }
    if (branch->audio_capsfilter) {
        LOG_I("unlink branch '%s': set audio capsfilter NULL", branch->output_id);
        gst_element_set_state(branch->audio_capsfilter, GST_STATE_NULL);
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
        branch->audio_appsrc = NULL;
        branch->audio_queue = NULL;
        branch->audio_convert = NULL;
        branch->audio_resample = NULL;
        branch->audio_encoder = NULL;
        branch->audio_parser = NULL;
        branch->audio_capsfilter = NULL;
        branch->muxer = NULL;
        branch->sink = NULL;
        return;
    }

    /* Remove from pipeline bin */
    LOG_I("unlink branch '%s': remove branch elements from pipeline", branch->output_id);
    if (branch->sink) gst_bin_remove(GST_BIN(mgr->pipeline), branch->sink);
    if (branch->muxer) gst_bin_remove(GST_BIN(mgr->pipeline), branch->muxer);
    if (branch->audio_capsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_capsfilter);
    if (branch->audio_parser) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_parser);
    if (branch->audio_encoder) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_encoder);
    if (branch->audio_resample) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_resample);
    if (branch->audio_convert) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_convert);
    if (branch->audio_queue) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_queue);
    if (branch->audio_appsrc) gst_bin_remove(GST_BIN(mgr->pipeline), branch->audio_appsrc);
    if (branch->video_pacer) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_pacer);
    if (branch->video_queue) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_queue);
    LOG_I("unlink branch '%s': remove complete", branch->output_id);

    branch->video_queue = NULL;
    branch->video_pacer = NULL;
    branch->audio_appsrc = NULL;
    branch->audio_queue = NULL;
    branch->audio_convert = NULL;
    branch->audio_resample = NULL;
    branch->audio_encoder = NULL;
    branch->audio_parser = NULL;
    branch->audio_capsfilter = NULL;
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
    mgr->direct_audio_branches = g_ptr_array_new();
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mgr->pipeline_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    pthread_mutex_init(&mgr->audio_mutex, NULL);

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
    g_ptr_array_free(mgr->direct_audio_branches, TRUE);

    teardown_pipeline(mgr);

    g_free(mgr->codec);
    g_free(mgr->encoder_override);
    g_free(mgr->parser_name);
    pthread_mutex_destroy(&mgr->audio_mutex);
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
    pthread_mutex_lock(&mgr->audio_mutex);
    g_ptr_array_set_size(mgr->direct_audio_branches, 0);
    pthread_mutex_unlock(&mgr->audio_mutex);
    g_hash_table_remove_all(mgr->branches);

    /* enc_config may contain pointers returned by get_config(), so duplicate
     * strings before freeing the manager-owned copies below. */
    char *new_codec_owned = g_strdup(new_codec);
    char *new_encoder_owned = g_strdup(enc_config->encoder);

    /* Update config */
    g_free(mgr->codec);
    mgr->codec = new_codec_owned;
    mgr->bitrate_kbps = new_bitrate;
    mgr->gop_size = enc_config->gop_size;
    mgr->gop_pattern = enc_config->gop_pattern;
    mgr->rc_mode = enc_config->rc_mode;
    g_free(mgr->encoder_override);
    mgr->encoder_override = new_encoder_owned;
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
    GstElement *audio_appsrc;
    uint64_t duration_ns;
    GstClockTime pts_ns;
    GstFlowReturn ret;
    GPtrArray *direct_appsrcs;

    if (!mgr || !msg || !audio_data) return;

    duration_ns = msg->duration_ns != UINT64_MAX
        ? msg->duration_ns
        : (msg->sample_rate > 0
            ? ((uint64_t)msg->n_samples * GST_SECOND) / msg->sample_rate
            : 10 * GST_MSECOND);

    pthread_mutex_lock(&mgr->audio_mutex);
    if (!mgr->pipeline_active || !mgr->audio_appsrc) {
        pthread_mutex_unlock(&mgr->audio_mutex);
        return;
    }
    audio_appsrc = gst_object_ref(mgr->audio_appsrc);
    pts_ns = next_audio_sample_time(mgr, msg, duration_ns);
    direct_appsrcs = g_ptr_array_new();
    if (mgr->direct_audio_branches) {
        for (guint i = 0; i < mgr->direct_audio_branches->len; i++) {
            sink_branch_t *branch = g_ptr_array_index(mgr->direct_audio_branches, i);
            if (!branch->direct_audio || !branch->audio_appsrc)
                continue;
            if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 &&
                (g_atomic_int_get(&branch->srt_caller_count) <= 0 ||
                 g_atomic_int_get(&branch->drop_until_keyframe)))
                continue;
            GstClockTime branch_pts = direct_audio_branch_next_pts(branch,
                                                                   pts_ns,
                                                                   duration_ns);
            g_ptr_array_add(direct_appsrcs, gst_object_ref(branch->audio_appsrc));
            g_ptr_array_add(direct_appsrcs, GUINT_TO_POINTER((guint)(branch_pts >> 32)));
            g_ptr_array_add(direct_appsrcs, GUINT_TO_POINTER((guint)(branch_pts & 0xffffffffu)));
        }
    }
    pthread_mutex_unlock(&mgr->audio_mutex);

    GstBuffer *buffer = make_audio_buffer(msg, audio_data, pts_ns, duration_ns);
    if (buffer)
        ret = gst_app_src_push_buffer(GST_APP_SRC(audio_appsrc), buffer);
    else
        ret = GST_FLOW_ERROR;
    gst_object_unref(audio_appsrc);

    for (guint i = 0; i + 2 < direct_appsrcs->len; i += 3) {
        GstElement *branch_appsrc = g_ptr_array_index(direct_appsrcs, i);
        uint64_t hi = (uint64_t)GPOINTER_TO_UINT(g_ptr_array_index(direct_appsrcs, i + 1));
        uint64_t lo = (uint64_t)GPOINTER_TO_UINT(g_ptr_array_index(direct_appsrcs, i + 2));
        GstClockTime branch_pts = (GstClockTime)((hi << 32) | lo);
        GstBuffer *branch_buffer = make_audio_buffer(msg, audio_data, branch_pts, duration_ns);
        if (!branch_buffer)
            continue;
        GstFlowReturn branch_ret = gst_app_src_push_buffer(GST_APP_SRC(branch_appsrc), branch_buffer);
        if (branch_ret != GST_FLOW_OK) {
            LOG_W("direct SRT audio appsrc push failed: %s", gst_flow_get_name(branch_ret));
        }
        gst_object_unref(branch_appsrc);
    }
    g_ptr_array_free(direct_appsrcs, TRUE);

    pthread_mutex_lock(&mgr->audio_mutex);
    if (ret == GST_FLOW_OK) {
        mgr->audio_buffers_pushed++;
        if (mgr->audio_buffers_pushed <= 3 || mgr->audio_buffers_pushed % 600 == 0) {
            LOG_I("encoder audio pushed #%lu samples=%u size=%u pts=%luns",
                  (unsigned long)mgr->audio_buffers_pushed,
                  msg->n_samples,
                  msg->data_size,
                  (unsigned long)pts_ns);
        }
    } else {
        mgr->audio_push_failures++;
        if (mgr->audio_push_failures <= 5 || mgr->audio_push_failures % 100 == 0)
            LOG_W("encoded audio appsrc push failed #%lu: %s",
                  (unsigned long)mgr->audio_push_failures,
                  gst_flow_get_name(ret));
    }
    pthread_mutex_unlock(&mgr->audio_mutex);
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
    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);
    branch->rtmp_uri     = g_strdup(config->rtmp_uri);
    branch->rtmp_passcode = g_strdup(config->rtmp_passcode);
    branch->file_path    = g_strdup(config->file_path);

    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0)
        rc = start_srt_session(mgr, branch);
    else
        rc = link_sink_branch(mgr, branch);
    if (rc != SBS_OK) {
        sink_branch_free(branch);
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return rc;
    }

    if (branch->direct_audio) {
        pthread_mutex_lock(&mgr->audio_mutex);
        g_ptr_array_add(mgr->direct_audio_branches, branch);
        pthread_mutex_unlock(&mgr->audio_mutex);
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
    pthread_mutex_lock(&mgr->audio_mutex);
    direct_audio_branch_remove(mgr, branch);
    pthread_mutex_unlock(&mgr->audio_mutex);
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
