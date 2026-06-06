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

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <srt/srt.h>

/* ── Sink Branch State ────────────────────────────────────────── */

typedef struct mux_feed_buffer {
    GstBuffer *buffer;
    bool is_video;
    GstClockTime sort_time;
    GstClockTime pts_time;
} mux_feed_buffer_t;

#define SBS_SRT_MUX_AUDIO_WAIT_NS (40ULL * GST_MSECOND)
#define SBS_SRT_MUX_AUDIO_TOLERANCE_NS (32ULL * GST_MSECOND)
#define SBS_SRT_MUX_AUDIO_MISSING_MAX_WAITS 5u

typedef struct sink_branch {
    char       *output_id;
    sbs_encoder_manager_t *manager;

    /* GStreamer elements (all owned by pipeline bin) */
    GstElement *video_queue;
    GstElement *video_parser;
    GstElement *video_capsfilter;
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
    gulong      sink_probe_id;
    gulong      flv_hevc_probe_id;
    gint        sink_buffers_seen;
    uint64_t    sink_bytes_seen;
    pthread_mutex_t sink_stats_mutex;
    bool        sink_stats_initialized;
    gint        sink_warning_count;
    gint        sink_error_count;

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
    uint64_t    srt_video_base_dts_ns;
    uint64_t    srt_video_running_origin_ns;
    uint64_t    srt_video_max_pts_ns;
    uint64_t    srt_video_buffers_pushed;
    uint64_t    srt_video_push_failures;
    bool        srt_stats_bytes_valid;
    uint64_t    srt_last_bytes_sent_total;
    uint64_t    srt_last_bytes_change_video_count;

    pthread_mutex_t srt_sender_mutex;
    pthread_t       srt_accept_thread;
    bool            srt_sender_initialized;
    bool            srt_sender_running;
    bool            srt_accept_started;
    SRTSOCKET       srt_listener;
    GArray         *srt_clients;
    GByteArray    *srt_ts_input;
    GByteArray    *srt_video_ts_backlog;
    GByteArray    *srt_send_payload;
    uint16_t        srt_listen_port;
    uint64_t        srt_sender_bytes_sent;
    uint64_t        srt_sender_send_failures;
    uint64_t        srt_sender_packets_sent;
    uint64_t        srt_sender_packets_dropped;

    pthread_mutex_t mux_feed_mutex;
    pthread_cond_t  mux_feed_cond;
    pthread_t       mux_feed_thread;
    bool            mux_feed_initialized;
    bool            mux_feed_running;
    bool            mux_feed_started;
    bool            mux_audio_coverage_valid;
    GstClockTime    mux_audio_pushed_until_ns;
    uint64_t        mux_audio_wait_warnings;
    uint64_t        mux_audio_missing_waits;
    gint            mux_video_only_mode;
    GQueue         *mux_video_queue;
    GQueue         *mux_audio_queue;

    GByteArray    *flv_hevc_pending;
    bool           flv_hevc_header_seen;
    bool           flv_hevc_hdr_metadata_sent;
    bool           flv_hevc_hdr_metadata_available;
    int            flv_hevc_hdr_bit_depth;
    int            flv_hevc_hdr_primaries;
    int            flv_hevc_hdr_transfer;
    int            flv_hevc_hdr_matrix;
    int            flv_hevc_hdr_max_luminance;
    bool           flv_hevc_legacy_mode;
    uint64_t       flv_hevc_tags_rewritten;

    /* Deep-copied config for potential restart */
    char       *sink_type;
    char       *rtmp_flv_mode;
    char       *srt_uri;
    char       *srt_mode;
    char       *srt_stream_key;
    char       *srt_passphrase;
    uint32_t    srt_latency_ms;
    char       *rtmp_uri;
    char       *rtmp_passcode;
    char       *file_path;
    char       *file_path_mode;
    char       *file_prefix;
    char       *file_container;
} sink_branch_t;

#define SBS_ENCODER_PACER_DELAY_NS (100ULL * GST_MSECOND)
#define SBS_SRT_AUDIO_BUFFER_NS (5ULL * GST_SECOND)
#define SBS_SRT_AUDIO_APP_MAX_BYTES (48000u * 2u * 2u * 5u)
#define SBS_TS_PACKET_SIZE 188u
#define SBS_TS_VIDEO_PID 0x100u
#define SBS_TS_AUDIO_PID 0x101u
#define SBS_SRT_VIDEO_BACKLOG_MAX_PACKETS 65536u
#define SBS_SRT_VIDEO_DRAIN_AFTER_AUDIO_PACKETS 98u

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

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(appsrc), "max-buffers"))
        g_object_set(appsrc, "max-buffers", max_buffers, NULL);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(appsrc), "leaky-type"))
        g_object_set(appsrc, "leaky-type", 2, NULL); /* downstream: drop oldest */
}

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
    sbs_pixel_format_t pixel_format;
    sbs_colorimetry_t colorimetry;

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
static bool update_srt_sink_stats(sink_branch_t *branch);
static GstClockTime element_running_time(GstElement *pipeline);
static GstFlowReturn on_srt_ts_sample(GstAppSink *appsink, gpointer user_data);
static int  start_custom_srt_sender(sink_branch_t *branch);
static void stop_custom_srt_sender(sink_branch_t *branch);

static bool schedule_srt_late_join_keyframe(sink_branch_t *branch,
                                            int32_t *gop_pattern_out,
                                            uint64_t *frames_pushed_out)
{
    sbs_encoder_manager_t *mgr = branch ? branch->manager : NULL;
    int32_t gop_pattern = -1;
    uint64_t frames_pushed = 0;
    bool force_idr = false;

    if (!mgr)
        return false;

    g_atomic_int_set(&branch->drop_until_keyframe, TRUE);

    pthread_mutex_lock(&mgr->pipeline_mutex);
    gop_pattern = mgr->gop_pattern;
    frames_pushed = mgr->frames_pushed;
    force_idr = true;
    mgr->force_next_idr = true;
    pthread_mutex_unlock(&mgr->pipeline_mutex);

    if (gop_pattern_out)
        *gop_pattern_out = gop_pattern;
    if (frames_pushed_out)
        *frames_pushed_out = frames_pushed;

    return force_idr;
}

static void mux_feed_buffer_free(gpointer data)
{
    mux_feed_buffer_t *item = data;
    if (!item)
        return;
    if (item->buffer)
        gst_buffer_unref(item->buffer);
    g_free(item);
}

static void mux_feed_clear_queue(GQueue *queue)
{
    if (!queue)
        return;
    while (!g_queue_is_empty(queue))
        mux_feed_buffer_free(g_queue_pop_head(queue));
}

static GstClockTime mux_feed_sort_time(GstBuffer *buffer, bool is_video)
{
    GstClockTime time;

    if (!buffer)
        return 0;

    if (is_video) {
        time = GST_BUFFER_DTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(time))
            return time;
    }

    time = GST_BUFFER_PTS(buffer);
    return GST_CLOCK_TIME_IS_VALID(time) ? time : 0;
}

static GstClockTime mux_feed_pts_time(GstBuffer *buffer)
{
    GstClockTime time;

    if (!buffer)
        return 0;

    time = GST_BUFFER_PTS(buffer);
    return GST_CLOCK_TIME_IS_VALID(time) ? time : 0;
}

static void mux_feed_wait_until(sink_branch_t *branch, GstClockTime target_time)
{
    if (!branch || !branch->srt_pipeline || !GST_CLOCK_TIME_IS_VALID(target_time))
        return;

    GstClockTime now = element_running_time(branch->srt_pipeline);
    if (!GST_CLOCK_TIME_IS_VALID(now) || target_time <= now)
        return;

    GstClockTime wait_ns = target_time - now;
    if (wait_ns > 100 * GST_MSECOND)
        wait_ns = 100 * GST_MSECOND;
    g_usleep((gulong)(wait_ns / 1000));
}

static void mux_feed_timedwait_ns(sink_branch_t *branch, uint64_t wait_ns)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(wait_ns / GST_SECOND);
    ts.tv_nsec += (long)(wait_ns % GST_SECOND);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_cond_timedwait(&branch->mux_feed_cond, &branch->mux_feed_mutex, &ts);
}

static void mux_feed_reset_locked(sink_branch_t *branch)
{
    if (!branch)
        return;
    mux_feed_clear_queue(branch->mux_video_queue);
    mux_feed_clear_queue(branch->mux_audio_queue);
    branch->mux_audio_coverage_valid = false;
    branch->mux_audio_pushed_until_ns = 0;
    branch->mux_audio_wait_warnings = 0;
    branch->mux_audio_missing_waits = 0;
    g_atomic_int_set(&branch->mux_video_only_mode, FALSE);
}

static void mux_feed_enqueue_buffer(sink_branch_t *branch,
                                    GstBuffer *buffer,
                                    bool is_video)
{
    mux_feed_buffer_t *item;

    if (!branch || !buffer || !branch->mux_feed_initialized) {
        if (buffer)
            gst_buffer_unref(buffer);
        return;
    }

    item = g_new0(mux_feed_buffer_t, 1);
    item->buffer = buffer;
    item->is_video = is_video;
    item->sort_time = mux_feed_sort_time(buffer, is_video);
    item->pts_time = mux_feed_pts_time(buffer);

    pthread_mutex_lock(&branch->mux_feed_mutex);
    if (!branch->mux_feed_running) {
        pthread_mutex_unlock(&branch->mux_feed_mutex);
        mux_feed_buffer_free(item);
        return;
    }

    g_queue_push_tail(is_video ? branch->mux_video_queue : branch->mux_audio_queue, item);
    pthread_cond_signal(&branch->mux_feed_cond);
    pthread_mutex_unlock(&branch->mux_feed_mutex);
}

static void *mux_feed_thread_main(void *data)
{
    sink_branch_t *branch = data;

    for (;;) {
        mux_feed_buffer_t *item = NULL;
        mux_feed_buffer_t *video;
        mux_feed_buffer_t *audio;

        pthread_mutex_lock(&branch->mux_feed_mutex);
        while (branch->mux_feed_running &&
               g_queue_is_empty(branch->mux_video_queue) &&
               g_queue_is_empty(branch->mux_audio_queue)) {
            pthread_cond_wait(&branch->mux_feed_cond, &branch->mux_feed_mutex);
        }

        if (!branch->mux_feed_running) {
            pthread_mutex_unlock(&branch->mux_feed_mutex);
            break;
        }

        video = g_queue_peek_head(branch->mux_video_queue);
        audio = g_queue_peek_head(branch->mux_audio_queue);
        if (audio && (!video || audio->sort_time <= video->pts_time)) {
            item = g_queue_pop_head(branch->mux_audio_queue);
        } else if (video) {
            bool audio_covers_video = branch->mux_audio_coverage_valid &&
                branch->mux_audio_pushed_until_ns + SBS_SRT_MUX_AUDIO_TOLERANCE_NS >= video->pts_time;
            if (!audio_covers_video && !audio) {
                if (branch->mux_audio_missing_waits < SBS_SRT_MUX_AUDIO_MISSING_MAX_WAITS) {
                    branch->mux_audio_missing_waits++;
                    mux_feed_timedwait_ns(branch, SBS_SRT_MUX_AUDIO_WAIT_NS);
                    pthread_mutex_unlock(&branch->mux_feed_mutex);
                    continue;
                }
                if (!g_atomic_int_get(&branch->mux_video_only_mode)) {
                    g_atomic_int_set(&branch->mux_video_only_mode, TRUE);
                    LOG_W("SRT session '%s' has no audio after %ums; feeding video-only until audio resumes",
                          branch->output_id ? branch->output_id : "<unknown>",
                          (unsigned)(SBS_SRT_MUX_AUDIO_MISSING_MAX_WAITS *
                                     (SBS_SRT_MUX_AUDIO_WAIT_NS / GST_MSECOND)));
                }
                branch->mux_audio_missing_waits++;
            } else if (!audio_covers_video && audio && audio->sort_time > video->pts_time) {
                branch->mux_audio_wait_warnings++;
                if (branch->mux_audio_wait_warnings <= 5 ||
                    branch->mux_audio_wait_warnings % 120 == 0) {
                    LOG_W("SRT session '%s' feeding video ahead of audio discontinuity #%lu video_time=%luns audio_next=%luns audio_until=%luns",
                          branch->output_id ? branch->output_id : "<unknown>",
                          (unsigned long)branch->mux_audio_wait_warnings,
                          (unsigned long)video->pts_time,
                          (unsigned long)audio->sort_time,
                          (unsigned long)(branch->mux_audio_coverage_valid
                              ? branch->mux_audio_pushed_until_ns : 0));
                }
            } else {
                branch->mux_audio_missing_waits = 0;
            }

            if (!item)
                item = g_queue_pop_head(branch->mux_video_queue);
        }
        pthread_mutex_unlock(&branch->mux_feed_mutex);

        if (!item)
            continue;

        GstElement *appsrc = item->is_video
            ? branch->srt_video_appsrc
            : branch->srt_audio_appsrc;
        GstClockTime pts = item->buffer ? GST_BUFFER_PTS(item->buffer) : GST_CLOCK_TIME_NONE;
        GstClockTime duration = item->buffer ? GST_BUFFER_DURATION(item->buffer) : GST_CLOCK_TIME_NONE;
        size_t size = item->buffer ? gst_buffer_get_size(item->buffer) : 0;
        GstFlowReturn ret;

        if (item->is_video)
            mux_feed_wait_until(branch, item->sort_time);

        if (appsrc) {
            ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), item->buffer);
            item->buffer = NULL; /* appsrc takes ownership, even on flow errors. */
        } else {
            ret = GST_FLOW_ERROR;
        }

        if (item->is_video) {
            if (ret == GST_FLOW_OK) {
                branch->srt_video_buffers_pushed++;
                if (branch->srt_video_buffers_pushed <= 3 ||
                    branch->srt_video_buffers_pushed % 120 == 0) {
                    if (!update_srt_sink_stats(branch)) {
                        mux_feed_buffer_free(item);
                        continue;
                    }
                }
                if (branch->srt_video_buffers_pushed <= 3 ||
                    branch->srt_video_buffers_pushed % 600 == 0) {
                    LOG_I("SRT session '%s' video pushed #%lu pts=%luns size=%zu",
                          branch->output_id,
                          (unsigned long)branch->srt_video_buffers_pushed,
                          (unsigned long)pts,
                          size);
                }
            } else {
                branch->srt_video_push_failures++;
                if (branch->srt_video_push_failures <= 5 ||
                    branch->srt_video_push_failures % 100 == 0) {
                    LOG_W("SRT session '%s' video appsrc push failed #%lu: %s",
                          branch->output_id,
                          (unsigned long)branch->srt_video_push_failures,
                          gst_flow_get_name(ret));
                }
            }
        } else {
            if (ret == GST_FLOW_OK) {
                GstClockTime end = GST_CLOCK_TIME_IS_VALID(pts) ? pts : item->sort_time;
                if (GST_CLOCK_TIME_IS_VALID(duration))
                    end += duration;

                pthread_mutex_lock(&branch->mux_feed_mutex);
                branch->mux_audio_missing_waits = 0;
                if (!branch->mux_audio_coverage_valid || end > branch->mux_audio_pushed_until_ns) {
                    branch->mux_audio_coverage_valid = true;
                    branch->mux_audio_pushed_until_ns = end;
                }
                pthread_mutex_unlock(&branch->mux_feed_mutex);
                g_atomic_int_set(&branch->mux_video_only_mode, FALSE);

                branch->direct_audio_buffers_pushed++;
                if (branch->direct_audio_buffers_pushed <= 3 ||
                    branch->direct_audio_buffers_pushed % 600 == 0) {
                    LOG_I("SRT session '%s' audio pushed #%lu pts=%luns size=%zu",
                          branch->output_id,
                          (unsigned long)branch->direct_audio_buffers_pushed,
                          (unsigned long)pts,
                          size);
                }
            } else {
                branch->direct_audio_push_failures++;
                if (branch->direct_audio_push_failures <= 5 ||
                    branch->direct_audio_push_failures % 100 == 0) {
                    LOG_W("SRT session '%s' audio appsrc push failed #%lu: %s",
                          branch->output_id,
                          (unsigned long)branch->direct_audio_push_failures,
                          gst_flow_get_name(ret));
                }
            }
        }

        mux_feed_buffer_free(item);
    }

    return NULL;
}

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
    int32_t gop_pattern = -1;
    uint64_t frames_pushed = 0;
    bool forced_idr = schedule_srt_late_join_keyframe(branch, &gop_pattern, &frames_pushed);

    LOG_I("SRT caller %d connected; callers=%d %s (gop_pattern=%d frames_pushed=%lu)",
          caller_id, g_atomic_int_get(&branch->srt_caller_count),
          forced_idr ? "scheduled IDR" : "waiting for natural B-frame keyframe",
          gop_pattern, (unsigned long)frames_pushed);
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

    if (g_atomic_int_get(&branch->srt_caller_count) <= 0)
        flush_srt_session(branch);

    int32_t gop_pattern = -1;
    uint64_t frames_pushed = 0;
    bool forced_idr = schedule_srt_late_join_keyframe(branch, &gop_pattern, &frames_pushed);

    LOG_I("SRT caller connecting; %s (gop_pattern=%d frames_pushed=%lu)",
          forced_idr ? "scheduled IDR" : "waiting for natural B-frame keyframe",
          gop_pattern, (unsigned long)frames_pushed);
    return TRUE;
}

static GstPadProbeReturn branch_sink_buffer_probe(GstPad *pad,
                                                  GstPadProbeInfo *info,
                                                  gpointer user_data)
{
    (void)pad;
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0)
        return GST_PAD_PROBE_OK;

    sink_branch_t *branch = user_data;
    if (branch) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (branch->sink_stats_initialized)
            pthread_mutex_lock(&branch->sink_stats_mutex);
        branch->sink_buffers_seen++;
        if (buffer)
            branch->sink_bytes_seen += gst_buffer_get_size(buffer);
        if (branch->sink_stats_initialized)
            pthread_mutex_unlock(&branch->sink_stats_mutex);
    }
    return GST_PAD_PROBE_OK;
}

static guint32 flv_read_u24(const guint8 *p)
{
    return ((guint32)p[0] << 16) | ((guint32)p[1] << 8) | (guint32)p[2];
}

static void flv_append_u24(GByteArray *out, guint32 value)
{
    guint8 bytes[3] = {
        (guint8)((value >> 16) & 0xffu),
        (guint8)((value >> 8) & 0xffu),
        (guint8)(value & 0xffu),
    };
    g_byte_array_append(out, bytes, sizeof(bytes));
}

static void flv_append_u16(GByteArray *out, guint16 value)
{
    guint8 bytes[2] = {
        (guint8)((value >> 8) & 0xffu),
        (guint8)(value & 0xffu),
    };
    g_byte_array_append(out, bytes, sizeof(bytes));
}

static void flv_append_u32(GByteArray *out, guint32 value)
{
    guint8 bytes[4] = {
        (guint8)((value >> 24) & 0xffu),
        (guint8)((value >> 16) & 0xffu),
        (guint8)((value >> 8) & 0xffu),
        (guint8)(value & 0xffu),
    };
    g_byte_array_append(out, bytes, sizeof(bytes));
}

static void flv_append_double(GByteArray *out, double value)
{
    guint64 bits = 0;
    guint8 bytes[8];
    memcpy(&bits, &value, sizeof(bits));
    bytes[0] = (guint8)((bits >> 56) & 0xffu);
    bytes[1] = (guint8)((bits >> 48) & 0xffu);
    bytes[2] = (guint8)((bits >> 40) & 0xffu);
    bytes[3] = (guint8)((bits >> 32) & 0xffu);
    bytes[4] = (guint8)((bits >> 24) & 0xffu);
    bytes[5] = (guint8)((bits >> 16) & 0xffu);
    bytes[6] = (guint8)((bits >> 8) & 0xffu);
    bytes[7] = (guint8)(bits & 0xffu);
    g_byte_array_append(out, bytes, sizeof(bytes));
}

static void flv_append_amf_string(GByteArray *out, const char *value)
{
    size_t len = value ? strlen(value) : 0;
    if (len > 65535)
        len = 65535;
    flv_append_u16(out, (guint16)len);
    if (len > 0)
        g_byte_array_append(out, (const guint8 *)value, (guint)len);
}

static void flv_append_amf_number_property(GByteArray *out,
                                           const char *name,
                                           double value)
{
    guint8 marker = 0x00; /* AMF0 number */
    flv_append_amf_string(out, name);
    g_byte_array_append(out, &marker, 1);
    flv_append_double(out, value);
}

static void flv_append_amf_object_start_property(GByteArray *out,
                                                 const char *name)
{
    guint8 marker = 0x03; /* AMF0 object */
    flv_append_amf_string(out, name);
    g_byte_array_append(out, &marker, 1);
}

static void flv_append_amf_object_end(GByteArray *out)
{
    guint8 end[3] = { 0x00, 0x00, 0x09 };
    g_byte_array_append(out, end, sizeof(end));
}

static void flv_append_tag(GByteArray *out,
                           const guint8 *tag,
                           const guint8 *payload,
                           guint32 payload_size)
{
    g_byte_array_append(out, tag, 1);
    flv_append_u24(out, payload_size);
    g_byte_array_append(out, tag + 4, 7);
    if (payload_size > 0)
        g_byte_array_append(out, payload, payload_size);
    flv_append_u32(out, 11u + payload_size);
}

static bool flv_payload_has_h265_fourcc(const guint8 *payload, guint32 payload_size)
{
    return payload_size >= 5 &&
           (payload[0] & 0x80u) != 0 &&
           (memcmp(payload + 1, "hvc1", 4) == 0 ||
            memcmp(payload + 1, "hev1", 4) == 0);
}

static bool flv_payload_is_avc_sequence_header(const guint8 *payload, guint32 payload_size)
{
    return payload_size >= 5 &&
           (payload[0] & 0x0fu) == 7u &&
           payload[1] == 0;
}

static bool flv_hevc_hdr_metadata_values_from_manager(const sbs_encoder_manager_t *mgr,
                                                      int *bit_depth,
                                                      int *primaries,
                                                      int *transfer,
                                                      int *matrix,
                                                      int *max_luminance)
{
    if (!mgr)
        return false;

    if (mgr->colorimetry == SBS_COLORIMETRY_BT2020_PQ || mgr->hdr10) {
        if (bit_depth) *bit_depth = mgr->pixel_format == SBS_PIXEL_FORMAT_P010 ? 10 : 8;
        if (primaries) *primaries = 9;  /* BT.2020 */
        if (transfer) *transfer = 16;   /* SMPTE ST 2084 / PQ */
        if (matrix) *matrix = 9;        /* BT.2020 non-constant luminance */
        if (max_luminance) *max_luminance = 1000;
        return true;
    }

    if (mgr->colorimetry == SBS_COLORIMETRY_BT2100_HLG) {
        if (bit_depth) *bit_depth = mgr->pixel_format == SBS_PIXEL_FORMAT_P010 ? 10 : 8;
        if (primaries) *primaries = 9;  /* BT.2020 */
        if (transfer) *transfer = 18;   /* ARIB STD-B67 / HLG */
        if (matrix) *matrix = 9;        /* BT.2020 non-constant luminance */
        if (max_luminance) *max_luminance = 1000;
        return true;
    }

    return false;
}

static void flv_hevc_cache_hdr_metadata_values(sink_branch_t *branch,
                                               const sbs_encoder_manager_t *mgr)
{
    int bit_depth = 10;
    int primaries = 9;
    int transfer = 16;
    int matrix = 9;
    int max_luminance = 1000;

    if (!branch)
        return;

    branch->flv_hevc_hdr_metadata_available =
        flv_hevc_hdr_metadata_values_from_manager(mgr, &bit_depth, &primaries,
                                                  &transfer, &matrix, &max_luminance);
    branch->flv_hevc_hdr_bit_depth = bit_depth;
    branch->flv_hevc_hdr_primaries = primaries;
    branch->flv_hevc_hdr_transfer = transfer;
    branch->flv_hevc_hdr_matrix = matrix;
    branch->flv_hevc_hdr_max_luminance = max_luminance;
}

static bool flv_hevc_hdr_metadata_values(const sink_branch_t *branch,
                                         int *bit_depth,
                                         int *primaries,
                                         int *transfer,
                                         int *matrix,
                                         int *max_luminance)
{
    if (branch && branch->flv_hevc_hdr_metadata_available) {
        if (bit_depth) *bit_depth = branch->flv_hevc_hdr_bit_depth;
        if (primaries) *primaries = branch->flv_hevc_hdr_primaries;
        if (transfer) *transfer = branch->flv_hevc_hdr_transfer;
        if (matrix) *matrix = branch->flv_hevc_hdr_matrix;
        if (max_luminance) *max_luminance = branch->flv_hevc_hdr_max_luminance;
        return true;
    }

    return flv_hevc_hdr_metadata_values_from_manager(
        branch ? branch->manager : NULL,
        bit_depth, primaries, transfer, matrix, max_luminance);
}

static GByteArray *flv_create_hevc_hdr_metadata_payload(const sink_branch_t *branch)
{
    int bit_depth = 10;
    int primaries = 9;
    int transfer = 16;
    int matrix = 9;
    int max_luminance = 1000;
    guint8 marker;
    GByteArray *payload;

    if (!flv_hevc_hdr_metadata_values(branch, &bit_depth, &primaries,
                                      &transfer, &matrix, &max_luminance))
        return NULL;

    payload = g_byte_array_new();

    marker = 0x02; /* AMF0 string */
    g_byte_array_append(payload, &marker, 1);
    flv_append_amf_string(payload, "colorInfo");

    marker = 0x03; /* AMF0 object */
    g_byte_array_append(payload, &marker, 1);

    flv_append_amf_object_start_property(payload, "colorConfig");
    flv_append_amf_number_property(payload, "bitDepth", bit_depth);
    flv_append_amf_number_property(payload, "colorPrimaries", primaries);
    flv_append_amf_number_property(payload, "transferCharacteristics", transfer);
    flv_append_amf_number_property(payload, "matrixCoefficients", matrix);
    flv_append_amf_object_end(payload);

    if (max_luminance > 0) {
        flv_append_amf_object_start_property(payload, "hdrMdcv");
        flv_append_amf_number_property(payload, "maxLuminance", max_luminance);
        flv_append_amf_number_property(payload, "minLuminance", 0);
        flv_append_amf_object_end(payload);
    }

    flv_append_amf_object_end(payload);
    return payload;
}

static void flv_append_hevc_hdr_metadata_tag(sink_branch_t *branch,
                                              const guint8 *reference_tag,
                                              GByteArray *out)
{
    GByteArray *metadata = flv_create_hevc_hdr_metadata_payload(branch);
    GByteArray *payload;
    guint8 ex_header[5] = { 0xd4, 'h', 'v', 'c', '1' };

    if (!metadata)
        return;

    payload = g_byte_array_sized_new(metadata->len + sizeof(ex_header));
    g_byte_array_append(payload, ex_header, sizeof(ex_header));
    g_byte_array_append(payload, metadata->data, metadata->len);
    flv_append_tag(out, reference_tag, payload->data, payload->len);

    g_byte_array_free(payload, TRUE);
    g_byte_array_free(metadata, TRUE);
    if (branch) {
        branch->flv_hevc_hdr_metadata_sent = true;
        LOG_I("RTMP H.265 FLV HDR metadata injected for '%s'",
              branch->output_id ? branch->output_id : "?");
    }
}

static void flv_append_avc_hdr_metadata_tag(sink_branch_t *branch,
                                            const guint8 *reference_tag,
                                            GByteArray *out)
{
    GByteArray *metadata = flv_create_hevc_hdr_metadata_payload(branch);
    GByteArray *payload;
    guint8 ex_header[5] = { 0xd4, 'a', 'v', 'c', '1' };

    if (!metadata)
        return;

    payload = g_byte_array_sized_new(metadata->len + sizeof(ex_header));
    g_byte_array_append(payload, ex_header, sizeof(ex_header));
    g_byte_array_append(payload, metadata->data, metadata->len);
    flv_append_tag(out, reference_tag, payload->data, payload->len);

    g_byte_array_free(payload, TRUE);
    g_byte_array_free(metadata, TRUE);
    if (branch) {
        branch->flv_hevc_hdr_metadata_sent = true;
        LOG_I("RTMP H.264 FLV HDR metadata injected for '%s'",
              branch->output_id ? branch->output_id : "?");
    }
}

static bool flv_patch_hevc_metadata(GByteArray *payload)
{
    static const guint8 key[] = {
        0x00, 0x0c,
        'v', 'i', 'd', 'e', 'o', 'c', 'o', 'd', 'e', 'c', 'i', 'd',
        0x00,
    };
    static const guint8 hvc1_double[] = { 0x41, 0xda, 0x1d, 0x98, 0xcc, 0x40, 0x00, 0x00 };
    static const guint8 hev1_double[] = { 0x41, 0xda, 0x19, 0x5d, 0x8c, 0x40, 0x00, 0x00 };
    static const guint8 legacy_double[] = { 0x40, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    bool patched = false;

    if (!payload || payload->len < sizeof(key) + sizeof(legacy_double))
        return false;

    for (guint i = 0; i + sizeof(key) + sizeof(legacy_double) <= payload->len; i++) {
        guint8 *value = payload->data + i + sizeof(key);
        if (memcmp(payload->data + i, key, sizeof(key)) != 0)
            continue;
        if (memcmp(value, hvc1_double, sizeof(hvc1_double)) == 0 ||
            memcmp(value, hev1_double, sizeof(hev1_double)) == 0) {
            memcpy(value, legacy_double, sizeof(legacy_double));
            patched = true;
        }
    }

    return patched;
}

static bool flv_rewrite_hevc_video_tag_legacy(sink_branch_t *branch,
                                              const guint8 *tag,
                                              guint32 payload_size,
                                              GByteArray *out)
{
    const guint8 *payload = tag + 11;
    guint8 frame_type;
    guint8 packet_type;
    guint8 legacy_header;
    GByteArray *converted;

    if (!flv_payload_has_h265_fourcc(payload, payload_size))
        return false;

    frame_type = (payload[0] >> 4) & 0x07u;
    packet_type = payload[0] & 0x0fu;
    legacy_header = (guint8)((frame_type << 4) | 0x0cu);
    converted = g_byte_array_sized_new(payload_size);

    switch (packet_type) {
    case 0: { /* SequenceStart: fourcc becomes PacketType + zero CTS. */
        guint8 header[5] = { legacy_header, 0, 0, 0, 0 };
        g_byte_array_append(converted, header, sizeof(header));
        if (payload_size > 5)
            g_byte_array_append(converted, payload + 5, payload_size - 5);
        break;
    }
    case 1: /* CodedFrames: keep CTS, remove the fourcc. */
        if (payload_size < 8) {
            g_byte_array_free(converted, TRUE);
            return false;
        }
        g_byte_array_append(converted, &legacy_header, 1);
        {
            guint8 legacy_packet_type = 1;
            g_byte_array_append(converted, &legacy_packet_type, 1);
        }
        g_byte_array_append(converted, payload + 5, 3);
        if (payload_size > 8)
            g_byte_array_append(converted, payload + 8, payload_size - 8);
        break;
    case 2: { /* SequenceEnd */
        guint8 header[5] = { legacy_header, 2, 0, 0, 0 };
        g_byte_array_append(converted, header, sizeof(header));
        if (payload_size > 5)
            g_byte_array_append(converted, payload + 5, payload_size - 5);
        break;
    }
    case 3: { /* CodedFramesX has no CTS; synthesize zero CTS for legacy FLV. */
        guint8 header[5] = { legacy_header, 1, 0, 0, 0 };
        g_byte_array_append(converted, header, sizeof(header));
        if (payload_size > 5)
            g_byte_array_append(converted, payload + 5, payload_size - 5);
        break;
    }
    default:
        g_byte_array_free(converted, TRUE);
        return false;
    }

    flv_append_tag(out, tag, converted->data, converted->len);
    if (branch)
        branch->flv_hevc_tags_rewritten++;
    g_byte_array_free(converted, TRUE);
    return true;
}

static bool flv_rewrite_hevc_video_tag_enhanced(sink_branch_t *branch,
                                                const guint8 *tag,
                                                guint32 payload_size,
                                                GByteArray *out)
{
    const guint8 *payload = tag + 11;
    guint8 packet_type;
    guint8 converted_header;
    GByteArray *converted;

    if (!flv_payload_has_h265_fourcc(payload, payload_size))
        return false;

    packet_type = payload[0] & 0x0fu;
    if (packet_type != 1 || payload_size < 8)
        return false;

    if (payload[5] != 0 || payload[6] != 0 || payload[7] != 0)
        return false;

    converted_header = (guint8)((payload[0] & 0xf0u) | 0x03u);
    converted = g_byte_array_sized_new(payload_size - 3u);
    g_byte_array_append(converted, &converted_header, 1);
    g_byte_array_append(converted, payload + 1, 4);
    if (payload_size > 8)
        g_byte_array_append(converted, payload + 8, payload_size - 8);

    flv_append_tag(out, tag, converted->data, converted->len);
    if (branch)
        branch->flv_hevc_tags_rewritten++;
    g_byte_array_free(converted, TRUE);
    return true;
}

static void flv_rewrite_complete_tag(sink_branch_t *branch,
                                     const guint8 *tag,
                                     guint32 payload_size,
                                     GByteArray *out)
{
    guint8 tag_type = tag[0];
    guint32 total_size = 11u + payload_size + 4u;

    if (tag_type == 9) {
        const guint8 *payload = tag + 11;
        if (branch && !branch->flv_hevc_legacy_mode &&
            !branch->flv_hevc_hdr_metadata_sent &&
            flv_payload_is_avc_sequence_header(payload, payload_size)) {
            flv_append_avc_hdr_metadata_tag(branch, tag, out);
            g_byte_array_append(out, tag, total_size);
            return;
        }

        if (!branch || !branch->flv_hevc_legacy_mode) {
            if (flv_payload_has_h265_fourcc(payload, payload_size) &&
                branch && !branch->flv_hevc_hdr_metadata_sent &&
                (payload[0] & 0x0fu) == 0) {
                g_byte_array_append(out, tag, total_size);
                flv_append_hevc_hdr_metadata_tag(branch, tag, out);
                return;
            }
            if (flv_rewrite_hevc_video_tag_enhanced(branch, tag, payload_size, out))
                return;
        } else if (flv_rewrite_hevc_video_tag_legacy(branch, tag, payload_size, out)) {
            return;
        }
    }

    if (branch && branch->flv_hevc_legacy_mode && tag_type == 18 && payload_size > 0) {
        GByteArray *payload = g_byte_array_sized_new(payload_size);
        g_byte_array_append(payload, tag + 11, payload_size);
        if (flv_patch_hevc_metadata(payload)) {
            flv_append_tag(out, tag, payload->data, payload->len);
            g_byte_array_free(payload, TRUE);
            return;
        }
        g_byte_array_free(payload, TRUE);
    }

    g_byte_array_append(out, tag, total_size);
}

static GByteArray *flv_rewrite_standalone_bytes(sink_branch_t *branch,
                                                const guint8 *data,
                                                gsize size)
{
    GByteArray *out;
    gsize processed = 0;

    if (!data || size == 0)
        return NULL;

    out = g_byte_array_sized_new(size + 256u);
    if (size >= 13 && memcmp(data, "FLV", 3) == 0) {
        g_byte_array_append(out, data, 13);
        processed = 13;
    }

    while (processed + 11 <= size) {
        const guint8 *tag = data + processed;
        guint8 tag_type = tag[0];
        guint32 payload_size;
        guint32 total_size;

        if (tag_type != 8 && tag_type != 9 && tag_type != 18)
            break;

        payload_size = flv_read_u24(tag + 1);
        total_size = 11u + payload_size + 4u;
        if (processed + total_size > size)
            break;

        flv_rewrite_complete_tag(branch, tag, payload_size, out);
        processed += total_size;
    }

    if (processed < size)
        g_byte_array_append(out, data + processed, size - processed);

    return out;
}

static GstBuffer *flv_rewrite_standalone_buffer(sink_branch_t *branch,
                                                GstBuffer *buffer)
{
    GstMapInfo map;
    GByteArray *out;
    GstBuffer *out_buffer;

    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ))
        return buffer ? gst_buffer_ref(buffer) : NULL;

    out = flv_rewrite_standalone_bytes(branch, map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    if (!out)
        return gst_buffer_ref(buffer);

    out_buffer = gst_buffer_new_allocate(NULL, out->len, NULL);
    if (!out_buffer) {
        g_byte_array_free(out, TRUE);
        return gst_buffer_ref(buffer);
    }

    gst_buffer_copy_into(out_buffer, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    gst_buffer_fill(out_buffer, 0, out->data, out->len);
    g_byte_array_free(out, TRUE);
    return out_buffer;
}

static GstPadProbeReturn flv_hevc_rewrite_caps_event(sink_branch_t *branch,
                                                     GstPadProbeInfo *info)
{
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    GstCaps *caps = NULL;
    GstCaps *new_caps;
    GstStructure *structure;
    const GValue *streamheader;
    GValue rewritten = G_VALUE_INIT;
    guint count;
    bool changed = false;

    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;

    gst_event_parse_caps(event, &caps);
    if (!caps || gst_caps_is_empty(caps))
        return GST_PAD_PROBE_OK;

    structure = gst_caps_get_structure(caps, 0);
    streamheader = gst_structure_get_value(structure, "streamheader");
    if (!streamheader || !GST_VALUE_HOLDS_ARRAY(streamheader))
        return GST_PAD_PROBE_OK;

    count = gst_value_array_get_size(streamheader);
    if (count == 0)
        return GST_PAD_PROBE_OK;

    g_value_init(&rewritten, GST_TYPE_ARRAY);
    for (guint i = 0; i < count; i++) {
        const GValue *entry = gst_value_array_get_value(streamheader, i);
        GstBuffer *buffer = entry ? gst_value_get_buffer(entry) : NULL;
        GstBuffer *rewritten_buffer = flv_rewrite_standalone_buffer(branch, buffer);
        GValue rewritten_entry = G_VALUE_INIT;

        if (!rewritten_buffer)
            continue;
        if (buffer)
            changed = true;

        g_value_init(&rewritten_entry, GST_TYPE_BUFFER);
        gst_value_set_buffer(&rewritten_entry, rewritten_buffer);
        gst_value_array_append_value(&rewritten, &rewritten_entry);
        g_value_unset(&rewritten_entry);
        gst_buffer_unref(rewritten_buffer);
    }

    if (!changed) {
        g_value_unset(&rewritten);
        return GST_PAD_PROBE_OK;
    }

    new_caps = gst_caps_copy(caps);
    structure = gst_caps_get_structure(new_caps, 0);
    gst_structure_set_value(structure, "streamheader", &rewritten);
    g_value_unset(&rewritten);

    GST_PAD_PROBE_INFO_DATA(info) = gst_event_new_caps(new_caps);
    gst_event_unref(event);
    gst_caps_unref(new_caps);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn flv_hevc_probe(GstPad *pad,
                                         GstPadProbeInfo *info,
                                         gpointer user_data)
{
    (void)pad;

    sink_branch_t *branch = user_data;
    GstBuffer *buffer;
    GstMapInfo map;
    GByteArray *pending;
    GByteArray *out;
    guint processed = 0;

    if (!branch)
        return GST_PAD_PROBE_OK;

    if ((info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0)
        return flv_hevc_rewrite_caps_event(branch, info);

    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0)
        return GST_PAD_PROBE_OK;

    buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_OK;

    if (!branch->flv_hevc_pending)
        branch->flv_hevc_pending = g_byte_array_new();
    pending = branch->flv_hevc_pending;
    g_byte_array_append(pending, map.data, map.size);
    gst_buffer_unmap(buffer, &map);

    out = g_byte_array_new();

    if (!branch->flv_hevc_header_seen) {
        if (pending->len < 13) {
            g_byte_array_free(out, TRUE);
            return GST_PAD_PROBE_DROP;
        }

        if (memcmp(pending->data, "FLV", 3) != 0) {
            LOG_W("RTMP H.265 FLV adapter did not see FLV header for '%s'; passing through",
                  branch->output_id ? branch->output_id : "?");
            g_byte_array_append(out, pending->data, pending->len);
            processed = pending->len;
            branch->flv_hevc_header_seen = true;
        } else {
            g_byte_array_append(out, pending->data, 13);
            processed = 13;
            branch->flv_hevc_header_seen = true;
        }
    }

    while (branch->flv_hevc_header_seen && processed + 11 <= pending->len) {
        const guint8 *tag = pending->data + processed;
        guint32 payload_size = flv_read_u24(tag + 1);
        guint32 total_size = 11u + payload_size + 4u;

        if (processed + total_size > pending->len)
            break;

        flv_rewrite_complete_tag(branch, tag, payload_size, out);
        processed += total_size;
    }

    if (processed > 0)
        g_byte_array_remove_range(pending, 0, processed);

    if (out->len == 0) {
        g_byte_array_free(out, TRUE);
        return GST_PAD_PROBE_DROP;
    }

    GstBuffer *out_buffer = gst_buffer_new_allocate(NULL, out->len, NULL);
    if (!out_buffer) {
        g_byte_array_free(out, TRUE);
        return GST_PAD_PROBE_DROP;
    }

    gst_buffer_copy_into(out_buffer, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    gst_buffer_fill(out_buffer, 0, out->data, out->len);
    g_byte_array_free(out, TRUE);

    gst_buffer_unref(buffer);
    GST_PAD_PROBE_INFO_DATA(info) = out_buffer;
    return GST_PAD_PROBE_OK;
}

static bool branch_owns_message_source(const sink_branch_t *branch,
                                       GstObject *source)
{
    if (!branch || !source)
        return false;

    GstObject *objects[] = {
        branch->video_queue ? GST_OBJECT(branch->video_queue) : NULL,
        branch->video_parser ? GST_OBJECT(branch->video_parser) : NULL,
        branch->video_capsfilter ? GST_OBJECT(branch->video_capsfilter) : NULL,
        branch->video_pacer ? GST_OBJECT(branch->video_pacer) : NULL,
        branch->audio_appsrc ? GST_OBJECT(branch->audio_appsrc) : NULL,
        branch->audio_queue ? GST_OBJECT(branch->audio_queue) : NULL,
        branch->audio_convert ? GST_OBJECT(branch->audio_convert) : NULL,
        branch->audio_resample ? GST_OBJECT(branch->audio_resample) : NULL,
        branch->audio_encoder ? GST_OBJECT(branch->audio_encoder) : NULL,
        branch->audio_parser ? GST_OBJECT(branch->audio_parser) : NULL,
        branch->audio_capsfilter ? GST_OBJECT(branch->audio_capsfilter) : NULL,
        branch->muxer ? GST_OBJECT(branch->muxer) : NULL,
        branch->sink ? GST_OBJECT(branch->sink) : NULL,
        branch->srt_pipeline ? GST_OBJECT(branch->srt_pipeline) : NULL,
        branch->srt_video_appsrc ? GST_OBJECT(branch->srt_video_appsrc) : NULL,
        branch->srt_audio_appsrc ? GST_OBJECT(branch->srt_audio_appsrc) : NULL,
    };

    for (size_t i = 0; i < G_N_ELEMENTS(objects); i++) {
        if (objects[i] == source)
            return true;
    }
    return false;
}

static void note_branch_bus_message(sbs_encoder_manager_t *mgr,
                                    GstObject *source,
                                    bool error)
{
    if (!mgr || !source)
        return;

    pthread_mutex_lock(&mgr->pipeline_mutex);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, mgr->branches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        sink_branch_t *branch = value;
        if (!branch_owns_message_source(branch, source))
            continue;
        if (error)
            g_atomic_int_inc(&branch->sink_error_count);
        else
            g_atomic_int_inc(&branch->sink_warning_count);
        break;
    }
    pthread_mutex_unlock(&mgr->pipeline_mutex);
}

static void ensure_srt_ready(void)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        srt_startup();
        g_once_init_leave(&initialized, 1);
    }
}

static bool srt_mode_is_caller(const char *mode)
{
    return mode &&
           (g_ascii_strcasecmp(mode, "caller") == 0 ||
            g_ascii_strcasecmp(mode, "client") == 0);
}

static bool srt_mode_is_listener(const char *mode)
{
    return !mode || !*mode ||
           g_ascii_strcasecmp(mode, "listener") == 0 ||
           g_ascii_strcasecmp(mode, "server") == 0;
}

static bool parse_srt_uri_endpoint(const char *uri,
                                   char *host,
                                   size_t host_size,
                                   uint16_t *port_out)
{
    const char *p;
    const char *end;
    const char *host_start;
    const char *host_end;
    const char *colon;
    char *port_end = NULL;
    long port;
    size_t host_len;

    if (!uri || !*uri || !port_out)
        return false;

    p = uri;
    if (strncmp(p, "srt://", 6) == 0)
        p += 6;

    end = p;
    while (*end && *end != '/' && *end != '?')
        end++;
    if (p == end)
        return false;

    if (*p == '[') {
        host_start = p + 1;
        host_end = memchr(host_start, ']', (size_t)(end - host_start));
        if (!host_end || host_end + 1 >= end || host_end[1] != ':')
            return false;
        colon = host_end + 1;
    } else {
        colon = NULL;
        for (const char *q = end; q > p; q--) {
            if (q[-1] == ':') {
                colon = q - 1;
                break;
            }
        }
        if (!colon)
            return false;
        host_start = p;
        host_end = colon;
    }

    if (colon + 1 >= end)
        return false;

    port = strtol(colon + 1, &port_end, 10);
    if (port <= 0 || port > 65535)
        return false;
    if (port_end != end)
        return false;

    host_len = (size_t)(host_end - host_start);
    if (host && host_size > 0) {
        if (host_len >= host_size)
            return false;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';
    }

    *port_out = (uint16_t)port;
    return true;
}

static uint16_t parse_srt_listen_port(const char *uri)
{
    uint16_t port = 0;
    if (!parse_srt_uri_endpoint(uri, NULL, 0, &port))
        return 0;
    return port;
}

static bool set_srt_sockopt(SRTSOCKET sock, SRT_SOCKOPT opt,
                            const void *value, int size,
                            const char *name)
{
    if (srt_setsockopt(sock, 0, opt, value, size) == 0)
        return true;

    LOG_W("custom SRT: failed to set %s: %s", name, srt_getlasterror_str());
    return false;
}

static int configure_srt_sender_socket(SRTSOCKET sock,
                                       uint32_t latency_ms,
                                       const char *stream_key,
                                       const char *passphrase,
                                       bool set_stream_id)
{
    SRT_TRANSTYPE transtype = SRTT_LIVE;
    int yes = 1;
    int payload = SRT_LIVE_DEF_PLSIZE;
    int timeout_ms = 20;
    int latency = latency_ms > 0 ? (int)latency_ms : 600;
    bool ok = true;

    ok &= set_srt_sockopt(sock, SRTO_TRANSTYPE, &transtype, sizeof(transtype), "SRTO_TRANSTYPE");
    ok &= set_srt_sockopt(sock, SRTO_REUSEADDR, &yes, sizeof(yes), "SRTO_REUSEADDR");
    ok &= set_srt_sockopt(sock, SRTO_SENDER, &yes, sizeof(yes), "SRTO_SENDER");
    ok &= set_srt_sockopt(sock, SRTO_TSBPDMODE, &yes, sizeof(yes), "SRTO_TSBPDMODE");
    ok &= set_srt_sockopt(sock, SRTO_PAYLOADSIZE, &payload, sizeof(payload), "SRTO_PAYLOADSIZE");
    ok &= set_srt_sockopt(sock, SRTO_LATENCY, &latency, sizeof(latency), "SRTO_LATENCY");
    ok &= set_srt_sockopt(sock, SRTO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms), "SRTO_SNDTIMEO");

    if (passphrase && *passphrase) {
        int pbkeylen = 16;
        ok &= set_srt_sockopt(sock, SRTO_PBKEYLEN, &pbkeylen, sizeof(pbkeylen), "SRTO_PBKEYLEN");
        ok &= set_srt_sockopt(sock, SRTO_PASSPHRASE, passphrase,
                              (int)strlen(passphrase), "SRTO_PASSPHRASE");
    }
    if (set_stream_id && stream_key && *stream_key) {
        ok &= set_srt_sockopt(sock, SRTO_STREAMID, stream_key,
                              (int)strlen(stream_key), "SRTO_STREAMID");
    }

    return ok ? SBS_OK : SBS_ERR_IO;
}

static bool custom_srt_schedule_keyframe(sink_branch_t *branch,
                                         int32_t *gop_pattern_out,
                                         uint64_t *frames_pushed_out)
{
    return schedule_srt_late_join_keyframe(branch, gop_pattern_out, frames_pushed_out);
}

static void custom_srt_client_connected(sink_branch_t *branch,
                                        SRTSOCKET client,
                                        guint caller_count)
{
    if (!branch)
        return;

    g_atomic_int_set(&branch->srt_caller_count, (gint)caller_count);
    if (caller_count == 1) {
        pthread_mutex_lock(&branch->srt_sender_mutex);
        if (branch->srt_video_ts_backlog)
            g_byte_array_set_size(branch->srt_video_ts_backlog, 0);
        if (branch->srt_ts_input)
            g_byte_array_set_size(branch->srt_ts_input, 0);
        if (branch->srt_send_payload)
            g_byte_array_set_size(branch->srt_send_payload, 0);
        pthread_mutex_unlock(&branch->srt_sender_mutex);
    }
    int32_t gop_pattern = -1;
    uint64_t frames_pushed = 0;
    bool forced_idr = custom_srt_schedule_keyframe(branch, &gop_pattern, &frames_pushed);
    LOG_I("custom SRT caller %d connected; callers=%u %s (gop_pattern=%d frames_pushed=%lu)",
          (int)client, caller_count,
          forced_idr ? "scheduled IDR" : "waiting for keyframe",
          gop_pattern, (unsigned long)frames_pushed);
}

static void custom_srt_clients_removed(sink_branch_t *branch,
                                       guint removed,
                                       guint caller_count)
{
    if (!branch || removed == 0)
        return;

    g_atomic_int_set(&branch->srt_caller_count, (gint)caller_count);
    if (caller_count == 0) {
        g_atomic_int_set(&branch->drop_until_keyframe, TRUE);
        pthread_mutex_lock(&branch->srt_sender_mutex);
        if (branch->srt_video_ts_backlog)
            g_byte_array_set_size(branch->srt_video_ts_backlog, 0);
        if (branch->srt_ts_input)
            g_byte_array_set_size(branch->srt_ts_input, 0);
        if (branch->srt_send_payload)
            g_byte_array_set_size(branch->srt_send_payload, 0);
        pthread_mutex_unlock(&branch->srt_sender_mutex);
        flush_srt_session(branch);
    }
    LOG_I("custom SRT callers removed=%u callers=%u", removed, caller_count);
}

static void *custom_srt_accept_thread_main(void *data)
{
    sink_branch_t *branch = data;

    while (branch) {
        SRTSOCKET listener;
        SRTSOCKET client;
        SRTSOCKET added_client = SRT_INVALID_SOCK;
        struct sockaddr_storage addr;
        int addrlen = sizeof(addr);
        guint caller_count = 0;
        bool running;

        pthread_mutex_lock(&branch->srt_sender_mutex);
        running = branch->srt_sender_running;
        listener = branch->srt_listener;
        pthread_mutex_unlock(&branch->srt_sender_mutex);
        if (!running || listener == SRT_INVALID_SOCK)
            break;

        client = srt_accept(listener, (struct sockaddr *)&addr, &addrlen);
        if (client == SRT_INVALID_SOCK) {
            pthread_mutex_lock(&branch->srt_sender_mutex);
            running = branch->srt_sender_running;
            pthread_mutex_unlock(&branch->srt_sender_mutex);
            if (!running)
                break;
            g_usleep(10000);
            continue;
        }

        pthread_mutex_lock(&branch->srt_sender_mutex);
        if (branch->srt_sender_running && branch->srt_clients) {
            g_array_append_val(branch->srt_clients, client);
            caller_count = branch->srt_clients->len;
            added_client = client;
            client = SRT_INVALID_SOCK;
        }
        pthread_mutex_unlock(&branch->srt_sender_mutex);

        if (client != SRT_INVALID_SOCK) {
            srt_close(client);
        } else {
            custom_srt_client_connected(branch, added_client, caller_count);
        }
    }

    return NULL;
}

static int init_custom_srt_sender_state(sink_branch_t *branch)
{
    if (!branch)
        return SBS_ERR_INVAL;

    if (!branch->srt_sender_initialized) {
        pthread_mutex_init(&branch->srt_sender_mutex, NULL);
        branch->srt_clients = g_array_new(FALSE, FALSE, sizeof(SRTSOCKET));
        branch->srt_ts_input = g_byte_array_new();
        branch->srt_video_ts_backlog = g_byte_array_new();
        branch->srt_send_payload = g_byte_array_new();
        branch->srt_listener = SRT_INVALID_SOCK;
        branch->srt_sender_initialized = true;
    }

    if (!branch->srt_clients || !branch->srt_ts_input ||
        !branch->srt_video_ts_backlog || !branch->srt_send_payload)
        return SBS_ERR_NOMEM;

    return SBS_OK;
}

static void reset_custom_srt_sender_locked(sink_branch_t *branch)
{
    if (!branch)
        return;

    if (branch->srt_clients)
        g_array_set_size(branch->srt_clients, 0);
    if (branch->srt_video_ts_backlog)
        g_byte_array_set_size(branch->srt_video_ts_backlog, 0);
    if (branch->srt_ts_input)
        g_byte_array_set_size(branch->srt_ts_input, 0);
    if (branch->srt_send_payload)
        g_byte_array_set_size(branch->srt_send_payload, 0);
    branch->srt_sender_bytes_sent = 0;
    branch->srt_sender_send_failures = 0;
    branch->srt_sender_packets_sent = 0;
    branch->srt_sender_packets_dropped = 0;
}

static int start_custom_srt_caller(sink_branch_t *branch)
{
    char host[256];
    char port_str[16];
    uint16_t port;
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *ai;
    SRTSOCKET client = SRT_INVALID_SOCK;
    SRTSOCKET connected_client;
    guint caller_count = 0;
    int rc;

    if (!branch)
        return SBS_ERR_INVAL;

    if (!parse_srt_uri_endpoint(branch->srt_uri, host, sizeof(host), &port) || host[0] == '\0') {
        LOG_E("custom SRT: invalid caller URI");
        return SBS_ERR_INVAL;
    }

    ensure_srt_ready();
    rc = init_custom_srt_sender_state(branch);
    if (rc != SBS_OK)
        return rc;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port_str, &hints, &results);
    if (rc != 0) {
        LOG_E("custom SRT: unable to resolve caller endpoint: %s", gai_strerror(rc));
        return SBS_ERR_IO;
    }

    for (ai = results; ai; ai = ai->ai_next) {
        client = srt_create_socket();
        if (client == SRT_INVALID_SOCK) {
            LOG_E("custom SRT: create socket failed: %s", srt_getlasterror_str());
            break;
        }
        rc = configure_srt_sender_socket(client, branch->srt_latency_ms,
                                         branch->srt_stream_key,
                                         branch->srt_passphrase,
                                         true);
        if (rc == SBS_OK && srt_connect(client, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        LOG_W("custom SRT: caller connect attempt failed: %s", srt_getlasterror_str());
        srt_close(client);
        client = SRT_INVALID_SOCK;
    }
    freeaddrinfo(results);

    if (client == SRT_INVALID_SOCK) {
        LOG_E("custom SRT: caller connect failed");
        return SBS_ERR_IO;
    }

    connected_client = client;
    pthread_mutex_lock(&branch->srt_sender_mutex);
    reset_custom_srt_sender_locked(branch);
    branch->srt_listener = SRT_INVALID_SOCK;
    branch->srt_listen_port = 0;
    branch->srt_sender_running = true;
    branch->srt_accept_started = false;
    g_array_append_val(branch->srt_clients, client);
    caller_count = branch->srt_clients->len;
    pthread_mutex_unlock(&branch->srt_sender_mutex);

    custom_srt_client_connected(branch, connected_client, caller_count);
    LOG_I("custom SRT sender connected in caller mode latency=%ums streamid=%s passphrase=%s",
          branch->srt_latency_ms > 0 ? branch->srt_latency_ms : 600,
          branch->srt_stream_key && *branch->srt_stream_key ? "set" : "not-set",
          branch->srt_passphrase && *branch->srt_passphrase ? "set" : "not-set");
    return SBS_OK;
}

static int start_custom_srt_sender(sink_branch_t *branch)
{
    struct sockaddr_in sa;
    SRTSOCKET listener;
    uint16_t port;
    int rc;

    if (!branch)
        return SBS_ERR_INVAL;

    if (srt_mode_is_caller(branch->srt_mode))
        return start_custom_srt_caller(branch);
    if (!srt_mode_is_listener(branch->srt_mode)) {
        LOG_E("custom SRT: unsupported srt_mode='%s'", branch->srt_mode ? branch->srt_mode : "<null>");
        return SBS_ERR_INVAL;
    }

    ensure_srt_ready();
    port = parse_srt_listen_port(branch->srt_uri);
    if (port == 0) {
        LOG_E("custom SRT: invalid listener URI");
        return SBS_ERR_INVAL;
    }

    rc = init_custom_srt_sender_state(branch);
    if (rc != SBS_OK)
        return rc;

    listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        LOG_E("custom SRT: create socket failed: %s", srt_getlasterror_str());
        return SBS_ERR_IO;
    }
    rc = configure_srt_sender_socket(listener, branch->srt_latency_ms,
                                     NULL, branch->srt_passphrase, false);
    if (rc != SBS_OK) {
        srt_close(listener);
        return rc;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (srt_bind(listener, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
        LOG_E("custom SRT: bind port %u failed: %s", port, srt_getlasterror_str());
        srt_close(listener);
        return SBS_ERR_IO;
    }
    if (srt_listen(listener, 8) == SRT_ERROR) {
        LOG_E("custom SRT: listen port %u failed: %s", port, srt_getlasterror_str());
        srt_close(listener);
        return SBS_ERR_IO;
    }

    pthread_mutex_lock(&branch->srt_sender_mutex);
    reset_custom_srt_sender_locked(branch);
    branch->srt_listener = listener;
    branch->srt_listen_port = port;
    branch->srt_sender_running = true;
    branch->srt_accept_started = false;
    pthread_mutex_unlock(&branch->srt_sender_mutex);

    if (pthread_create(&branch->srt_accept_thread, NULL,
                       custom_srt_accept_thread_main, branch) != 0) {
        LOG_E("custom SRT: failed to start accept thread");
        stop_custom_srt_sender(branch);
        return SBS_ERR_IO;
    }
    branch->srt_accept_started = true;
    LOG_I("custom SRT sender listening on port %u latency=%ums passphrase=%s", port,
          branch->srt_latency_ms > 0 ? branch->srt_latency_ms : 600,
          branch->srt_passphrase && *branch->srt_passphrase ? "set" : "not-set");
    return SBS_OK;
}

static void stop_custom_srt_sender(sink_branch_t *branch)
{
    SRTSOCKET listener = SRT_INVALID_SOCK;

    if (!branch || !branch->srt_sender_initialized)
        return;

    pthread_mutex_lock(&branch->srt_sender_mutex);
    branch->srt_sender_running = false;
    listener = branch->srt_listener;
    branch->srt_listener = SRT_INVALID_SOCK;
    if (branch->srt_clients) {
        for (guint i = 0; i < branch->srt_clients->len; i++) {
            SRTSOCKET client = g_array_index(branch->srt_clients, SRTSOCKET, i);
            if (client != SRT_INVALID_SOCK)
                srt_close(client);
        }
        g_array_set_size(branch->srt_clients, 0);
    }
    if (branch->srt_video_ts_backlog)
        g_byte_array_set_size(branch->srt_video_ts_backlog, 0);
    if (branch->srt_ts_input)
        g_byte_array_set_size(branch->srt_ts_input, 0);
    if (branch->srt_send_payload)
        g_byte_array_set_size(branch->srt_send_payload, 0);
    pthread_mutex_unlock(&branch->srt_sender_mutex);

    if (listener != SRT_INVALID_SOCK)
        srt_close(listener);
    if (branch->srt_accept_started) {
        pthread_join(branch->srt_accept_thread, NULL);
        branch->srt_accept_started = false;
    }
    g_atomic_int_set(&branch->srt_caller_count, 0);
}

static guint ts_packet_pid(const uint8_t *packet)
{
    return ((guint)(packet[1] & 0x1f) << 8) | (guint)packet[2];
}

static void custom_srt_send_message_locked(sink_branch_t *branch,
                                           const uint8_t *data,
                                           gsize size,
                                           guint *removed)
{
    if (!branch || !data || size == 0 || !branch->srt_clients)
        return;

    for (gint i = (gint)branch->srt_clients->len - 1; i >= 0; i--) {
        SRTSOCKET client = g_array_index(branch->srt_clients, SRTSOCKET, (guint)i);
        int len = (int)size;
        uint64_t packets = (uint64_t)((size + SBS_TS_PACKET_SIZE - 1) / SBS_TS_PACKET_SIZE);
        int rc = srt_sendmsg(client, (const char *)data, len, -1, 0);
        if (rc == SRT_ERROR || rc != len) {
            branch->srt_sender_send_failures++;
            branch->srt_sender_packets_dropped += packets;
            srt_close(client);
            g_array_remove_index_fast(branch->srt_clients, (guint)i);
            if (removed)
                (*removed)++;
        } else {
            branch->srt_sender_bytes_sent += size;
            branch->srt_sender_packets_sent += packets;
        }
    }
}

static void custom_srt_flush_payload_locked(sink_branch_t *branch,
                                            guint *removed,
                                            bool force)
{
    if (!branch || !branch->srt_send_payload)
        return;

    while (branch->srt_send_payload->len >= (guint)SRT_LIVE_DEF_PLSIZE ||
           (force && branch->srt_send_payload->len > 0)) {
        guint bytes = MIN((guint)SRT_LIVE_DEF_PLSIZE, branch->srt_send_payload->len);
        custom_srt_send_message_locked(branch, branch->srt_send_payload->data, bytes, removed);
        g_byte_array_remove_range(branch->srt_send_payload, 0, bytes);
        if (!branch->srt_clients || branch->srt_clients->len == 0)
            break;
    }
}

static void custom_srt_send_bytes_locked(sink_branch_t *branch,
                                         const uint8_t *data,
                                         gsize size,
                                         guint *removed)
{
    if (!branch || !data || size == 0 || !branch->srt_clients || branch->srt_clients->len == 0)
        return;

    if (!branch->srt_send_payload)
        branch->srt_send_payload = g_byte_array_new();

    g_byte_array_append(branch->srt_send_payload, data, size);
    custom_srt_flush_payload_locked(branch, removed, false);
}

static void custom_srt_send_video_backlog_locked(sink_branch_t *branch,
                                                 guint max_packets,
                                                 guint *removed)
{
    guint packets;
    guint bytes;

    if (!branch || !branch->srt_video_ts_backlog || branch->srt_video_ts_backlog->len == 0)
        return;

    packets = branch->srt_video_ts_backlog->len / SBS_TS_PACKET_SIZE;
    if (max_packets > 0 && packets > max_packets)
        packets = max_packets;
    bytes = packets * SBS_TS_PACKET_SIZE;
    if (bytes == 0)
        return;

    custom_srt_send_bytes_locked(branch, branch->srt_video_ts_backlog->data, bytes, removed);
    g_byte_array_remove_range(branch->srt_video_ts_backlog, 0, bytes);
}

static void custom_srt_send_interleaved_locked(sink_branch_t *branch,
                                               const uint8_t *data,
                                               gsize size,
                                               guint *removed)
{
    if (!branch || !data || size == 0 || !branch->srt_clients || branch->srt_clients->len == 0)
        return;

    if (!branch->srt_video_ts_backlog)
        branch->srt_video_ts_backlog = g_byte_array_new();
    if (!branch->srt_ts_input)
        branch->srt_ts_input = g_byte_array_new();

    g_byte_array_append(branch->srt_ts_input, data, size);

    while (branch->srt_ts_input->len >= SBS_TS_PACKET_SIZE) {
        const uint8_t *packet = branch->srt_ts_input->data;
        guint pid;

        if (packet[0] != 0x47) {
            guint sync = 1;
            while (sync < branch->srt_ts_input->len && branch->srt_ts_input->data[sync] != 0x47)
                sync++;
            if (sync >= branch->srt_ts_input->len) {
                g_byte_array_set_size(branch->srt_ts_input, 0);
                return;
            }
            g_byte_array_remove_range(branch->srt_ts_input, 0, sync);
            continue;
        }

        pid = ts_packet_pid(packet);
        if (pid == SBS_TS_VIDEO_PID) {
            g_byte_array_append(branch->srt_video_ts_backlog, packet, SBS_TS_PACKET_SIZE);
            if (g_atomic_int_get(&branch->mux_video_only_mode)) {
                custom_srt_send_video_backlog_locked(branch,
                                                    SBS_SRT_VIDEO_DRAIN_AFTER_AUDIO_PACKETS,
                                                    removed);
            } else {
                while (branch->srt_video_ts_backlog->len / SBS_TS_PACKET_SIZE >
                       SBS_SRT_VIDEO_BACKLOG_MAX_PACKETS) {
                    custom_srt_send_video_backlog_locked(branch, 7, removed);
                }
            }
        } else if (pid == SBS_TS_AUDIO_PID) {
            custom_srt_send_bytes_locked(branch, packet, SBS_TS_PACKET_SIZE, removed);
            custom_srt_send_video_backlog_locked(branch,
                                                SBS_SRT_VIDEO_DRAIN_AFTER_AUDIO_PACKETS,
                                                removed);
        } else {
            custom_srt_send_bytes_locked(branch, packet, SBS_TS_PACKET_SIZE, removed);
        }
        g_byte_array_remove_range(branch->srt_ts_input, 0, SBS_TS_PACKET_SIZE);
    }
}

static GstFlowReturn on_srt_ts_sample(GstAppSink *appsink, gpointer user_data)
{
    sink_branch_t *branch = user_data;
    GstSample *sample;
    GstBuffer *buffer;
    GstMapInfo map;
    guint removed = 0;
    guint caller_count = 0;

    if (!branch)
        return GST_FLOW_ERROR;

    sample = gst_app_sink_pull_sample(appsink);
    if (!sample)
        return GST_FLOW_OK;

    buffer = gst_sample_get_buffer(sample);
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    pthread_mutex_lock(&branch->srt_sender_mutex);
    if (branch->srt_sender_running && branch->srt_clients && branch->srt_clients->len > 0) {
        custom_srt_send_interleaved_locked(branch, map.data, map.size, &removed);
        caller_count = branch->srt_clients->len;
    } else if (branch->srt_video_ts_backlog) {
        g_byte_array_set_size(branch->srt_video_ts_backlog, 0);
        if (branch->srt_ts_input)
            g_byte_array_set_size(branch->srt_ts_input, 0);
        if (branch->srt_send_payload)
            g_byte_array_set_size(branch->srt_send_payload, 0);
    }
    pthread_mutex_unlock(&branch->srt_sender_mutex);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    if (removed > 0)
        custom_srt_clients_removed(branch, removed, caller_count);

    return GST_FLOW_OK;
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

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        note_branch_bus_message(user_data, GST_MESSAGE_SRC(msg), true);
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
        note_branch_bus_message(user_data, GST_MESSAGE_SRC(msg), false);
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

static void flush_srt_session(sink_branch_t *branch)
{
    sbs_encoder_manager_t *mgr = branch ? branch->manager : NULL;

    if (!branch || !branch->srt_pipeline)
        return;

    branch->srt_video_base_valid = false;
    branch->srt_video_base_pts_ns = 0;
    branch->srt_video_running_origin_ns = 0;
    branch->srt_video_max_pts_ns = 0;
    branch->srt_stats_bytes_valid = false;
    branch->srt_last_bytes_sent_total = 0;
    branch->srt_last_bytes_change_video_count = 0;
    if (branch->mux_feed_initialized) {
        pthread_mutex_lock(&branch->mux_feed_mutex);
        mux_feed_reset_locked(branch);
        pthread_cond_signal(&branch->mux_feed_cond);
        pthread_mutex_unlock(&branch->mux_feed_mutex);
    }
    if (mgr) pthread_mutex_lock(&mgr->audio_mutex);
    branch->direct_audio_base_valid = false;
    branch->direct_audio_next_valid = false;
    branch->direct_audio_base_pts_ns = 0;
    branch->direct_audio_next_pts_ns = 0;
    branch->direct_audio_buffers_pushed = 0;
    if (mgr) pthread_mutex_unlock(&mgr->audio_mutex);

    LOG_I("SRT session '%s' flushed", branch->output_id);
}

static bool update_srt_sink_stats(sink_branch_t *branch)
{
    GstStructure *stats = NULL;
    guint64 bytes_sent_total = 0;
    uint64_t stale_frames;
    gboolean have_bytes;

    if (!branch || g_atomic_int_get(&branch->srt_caller_count) <= 0)
        return false;

    if (branch->srt_sender_initialized) {
        pthread_mutex_lock(&branch->srt_sender_mutex);
        bytes_sent_total = branch->srt_sender_bytes_sent;
        pthread_mutex_unlock(&branch->srt_sender_mutex);
        have_bytes = TRUE;
        goto have_total;
    }

    if (!branch->sink)
        return false;

    g_object_get(branch->sink, "stats", &stats, NULL);
    if (!stats)
        return true;

    have_bytes = gst_structure_get_uint64(stats, "bytes-sent-total", &bytes_sent_total);
    gst_structure_free(stats);
    if (!have_bytes)
        return true;

have_total:
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
        LOG_W("SRT session '%s' caller stats stale: no bytes sent for %lu video frames; keeping caller active",
              branch->output_id ? branch->output_id : "<unknown>",
              (unsigned long)stale_frames);
        branch->srt_last_bytes_change_video_count = branch->srt_video_buffers_pushed;
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
    uint64_t reorder_delay_ns = mgr && mgr->video_encoder
        ? sbs_direct_venc_reorder_delay_ns(mgr->video_encoder)
        : 0;

    if (timestamp_ns == UINT64_MAX) {
        return pipeline_running_time(mgr) + SBS_ENCODER_PACER_DELAY_NS + reorder_delay_ns;
    }

    if (!mgr->audio_time_origin_valid) {
        mgr->audio_pts_origin_ns = timestamp_ns;
        mgr->audio_running_origin_ns = pipeline_running_time(mgr) +
                                       SBS_ENCODER_PACER_DELAY_NS +
                                       reorder_delay_ns;
        mgr->audio_time_origin_valid = true;
        LOG_I("encoded audio clock origin: pts=%luns running=%luns reorder_delay=%luns",
              (unsigned long)mgr->audio_pts_origin_ns,
              (unsigned long)mgr->audio_running_origin_ns,
              (unsigned long)reorder_delay_ns);
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
        branch->srt_video_base_dts_ns = GST_CLOCK_TIME_IS_VALID(dts_ns) ? dts_ns : pts_ns;
        branch->srt_video_running_origin_ns = running_origin;
        branch->srt_video_max_pts_ns = running_origin;
        branch->srt_video_buffers_pushed = 0;
        branch->srt_stats_bytes_valid = false;
        branch->srt_last_bytes_sent_total = 0;
        branch->srt_last_bytes_change_video_count = 0;
        pthread_mutex_lock(&branch->mux_feed_mutex);
        mux_feed_reset_locked(branch);
        pthread_cond_signal(&branch->mux_feed_cond);
        pthread_mutex_unlock(&branch->mux_feed_mutex);
        pthread_mutex_lock(&mgr->audio_mutex);
        branch->direct_audio_base_valid = true;
        branch->direct_audio_next_valid = false;
        branch->direct_audio_base_pts_ns = running_origin +
            (pts_ns >= branch->srt_video_base_dts_ns
                ? pts_ns - branch->srt_video_base_dts_ns
                : 0);
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
        (pts_ns >= branch->srt_video_base_dts_ns
            ? pts_ns - branch->srt_video_base_dts_ns
            : 0);
    if (rel_pts > branch->srt_video_max_pts_ns)
        branch->srt_video_max_pts_ns = rel_pts;
    rel_dts = GST_CLOCK_TIME_IS_VALID(dts_ns)
        ? branch->srt_video_running_origin_ns +
            (dts_ns >= branch->srt_video_base_dts_ns
                ? dts_ns - branch->srt_video_base_dts_ns
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

    mux_feed_enqueue_buffer(branch, buffer, true);
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
        .input_format = mgr->pixel_format,
        .colorimetry = mgr->colorimetry,
        .hdr10 = mgr->hdr10,
        .input_hdr10 = mgr->pixel_format == SBS_PIXEL_FORMAT_P010,
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
        NULL);
    set_appsrc_queue_limits(appsrc, (guint64)(mgr->bitrate_kbps * 1000), 8u);
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
    g_free(branch->srt_mode);
    g_free(branch->srt_stream_key);
    g_free(branch->srt_passphrase);
    g_free(branch->rtmp_uri);
    g_free(branch->rtmp_passcode);
    g_free(branch->rtmp_flv_mode);
    g_free(branch->file_path);
    g_free(branch->file_path_mode);
    g_free(branch->file_prefix);
    g_free(branch->file_container);
    if (branch->srt_callers)
        g_hash_table_destroy(branch->srt_callers);
    if (branch->sink_stats_initialized)
        pthread_mutex_destroy(&branch->sink_stats_mutex);
    if (branch->flv_hevc_pending)
        g_byte_array_free(branch->flv_hevc_pending, TRUE);
    stop_custom_srt_sender(branch);
    if (branch->srt_sender_initialized) {
        if (branch->srt_clients)
            g_array_free(branch->srt_clients, TRUE);
        if (branch->srt_video_ts_backlog)
            g_byte_array_free(branch->srt_video_ts_backlog, TRUE);
        if (branch->srt_ts_input)
            g_byte_array_free(branch->srt_ts_input, TRUE);
        if (branch->srt_send_payload)
            g_byte_array_free(branch->srt_send_payload, TRUE);
        pthread_mutex_destroy(&branch->srt_sender_mutex);
    }
    if (branch->mux_feed_initialized) {
        pthread_mutex_lock(&branch->mux_feed_mutex);
        branch->mux_feed_running = false;
        pthread_cond_signal(&branch->mux_feed_cond);
        pthread_mutex_unlock(&branch->mux_feed_mutex);
        if (branch->mux_feed_started)
            pthread_join(branch->mux_feed_thread, NULL);
        mux_feed_clear_queue(branch->mux_video_queue);
        mux_feed_clear_queue(branch->mux_audio_queue);
        g_queue_free(branch->mux_video_queue);
        g_queue_free(branch->mux_audio_queue);
        pthread_cond_destroy(&branch->mux_feed_cond);
        pthread_mutex_destroy(&branch->mux_feed_mutex);
    }
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

static const char *normalized_file_container(const char *container)
{
    if (container && (strcmp(container, "mkv") == 0 ||
                      strcmp(container, "flv") == 0 ||
                      strcmp(container, "mp4") == 0 ||
                      strcmp(container, "ts") == 0)) {
        return container;
    }
    return "ts";
}

static const char *normalized_rtmp_flv_mode(const char *mode)
{
    const char *value = (mode && *mode) ? mode : getenv("SBS_RTMP_HEVC_FLV_MODE");
    if (!value || !*value)
        return "enhanced";
    if (g_ascii_strcasecmp(value, "legacy") == 0 ||
        g_ascii_strcasecmp(value, "codec12") == 0 ||
        g_ascii_strcasecmp(value, "x-hevc") == 0)
        return "legacy";
    return "enhanced";
}

static char *sanitize_file_prefix(const char *prefix)
{
    char *safe = g_strdup((prefix && *prefix) ? prefix : "stream");
    for (char *p = safe; *p; p++) {
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 0x20) {
            *p = '_';
        }
    }
    return safe;
}

static char *resolve_file_location(const sbs_sink_branch_config_t *config)
{
    const char *container = normalized_file_container(config->file_container);
    const char *mode = config->file_path_mode ? config->file_path_mode : "file";
    const char *path = config->file_path;

    if (!path || !*path)
        return NULL;

    if (strcmp(mode, "directory") != 0)
        return g_strdup(path);

    if (g_mkdir_with_parents(path, 0755) != 0) {
        LOG_W("failed to create file output directory '%s'", path);
    }

    time_t now = time(NULL);
    struct tm tm_now;
    char timestamp[32];
    char *prefix = sanitize_file_prefix(config->file_prefix);
    if (!localtime_r(&now, &tm_now) ||
        strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm_now) == 0) {
        g_strlcpy(timestamp, "unknown-time", sizeof(timestamp));
    }
    char *filename = g_strdup_printf("%s-%s.%s", prefix, timestamp, container);
    char *location = g_build_filename(path, filename, NULL);
    g_free(prefix);
    g_free(filename);
    return location;
}

static GstElement *create_muxer(const char *codec, const char *sink_type,
                                 const char *file_container,
                                 const char **out_format)
{
    (void)codec;
    bool is_rtmp = (sink_type && strcmp(sink_type, "rtmp") == 0);
    bool is_file = (sink_type && strcmp(sink_type, "file") == 0);

    *out_format = "mpegts";
    GstElement *muxer;
    if (is_file) {
        const char *container = normalized_file_container(file_container);
        if (strcmp(container, "mkv") == 0) {
            muxer = gst_element_factory_make("matroskamux", NULL);
            *out_format = "mkv";
        } else if (strcmp(container, "flv") == 0) {
            muxer = gst_element_factory_make("flvmux", NULL);
            if (muxer) {
                g_object_set(muxer,
                    "streamable", TRUE,
                    "latency",    (guint64)100000000,
                    NULL);
            }
            *out_format = "flv";
        } else if (strcmp(container, "mp4") == 0) {
            muxer = gst_element_factory_make("mp4mux", NULL);
            *out_format = "mp4";
        } else {
            muxer = gst_element_factory_make("mpegtsmux", NULL);
            if (muxer) {
                g_object_set(muxer,
                    "alignment", (gint)7,
                    "latency",   (guint64)100000000,
                    NULL);
            }
            *out_format = "mpegts";
        }
    } else if (is_rtmp) {
        muxer = gst_element_factory_make("flvmux", NULL);
        if (muxer) {
            g_object_set(muxer,
                "streamable", TRUE,
                "latency",    (guint64)100000000,
                NULL);
        }
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

    (void)muxer_format;

    if (sink_type && strcmp(sink_type, "fakesink") == 0) {
        sink = gst_element_factory_make("fakesink", NULL);
        if (sink) {
            g_object_set(sink, "sync", FALSE, NULL);
            LOG_I("fakesink selected for output");
        }
    } else if (!sink_type || strcmp(sink_type, "srt") == 0) {
        if (config->srt_uri && strlen(config->srt_uri) > 0) {
            gint srt_mode;
            if (srt_mode_is_caller(config->srt_mode)) {
                srt_mode = 1;
            } else if (srt_mode_is_listener(config->srt_mode)) {
                srt_mode = 2;
            } else {
                LOG_E("SRT sink requested with unsupported srt_mode='%s'",
                      config->srt_mode ? config->srt_mode : "<null>");
                return NULL;
            }
            sink = gst_element_factory_make("srtserversink", NULL);
            if (!sink)
                sink = gst_element_factory_make("srtsink", NULL);
            if (sink) {
                uint32_t latency_ms = config->srt_latency_ms > 0
                    ? config->srt_latency_ms : 600;
                g_object_set(sink,
                    "uri",                 config->srt_uri,
                    "mode",                srt_mode,
                    "wait-for-connection", FALSE,
                    "poll-timeout",        (gint)100,
                    "latency",             (gint)latency_ms,
                    "blocksize",           (guint)1316,
                    "sync",                FALSE,
                    "async",               FALSE,
                    NULL);
                if (config->srt_stream_key && *config->srt_stream_key &&
                    g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "streamid")) {
                    g_object_set(sink, "streamid", config->srt_stream_key, NULL);
                }
                if (config->srt_passphrase && *config->srt_passphrase) {
                    if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "pbkeylen"))
                        g_object_set(sink, "pbkeylen", (gint)16, NULL);
                    if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "passphrase"))
                        g_object_set(sink, "passphrase", config->srt_passphrase, NULL);
                }
                LOG_I("SRT sink: mode=%s latency=%ums sync=0 blocksize=1316 streamid=%s passphrase=%s",
                      srt_mode == 1 ? "caller" : "listener",
                      latency_ms,
                      config->srt_stream_key && *config->srt_stream_key ? "set" : "not-set",
                      config->srt_passphrase && *config->srt_passphrase ? "set" : "not-set");
            }
        } else {
            LOG_E("SRT sink requested without srt_uri");
        }
    } else if (strcmp(sink_type, "rtmp") == 0) {
        if (config->rtmp_uri && strlen(config->rtmp_uri) > 0) {
            char *location = build_rtmp_location(config->rtmp_uri, config->rtmp_passcode);
            sink = gst_element_factory_make("rtmp2sink", NULL);
            if (!sink) sink = gst_element_factory_make("rtmpsink", NULL);
            if (sink) {
                g_object_set(sink,
                    "location", location ? location : config->rtmp_uri,
                    "sync",     FALSE,
                    "async",    FALSE,
                    NULL);
                LOG_I("RTMP sink: %s%s", config->rtmp_uri,
                      config->rtmp_passcode && *config->rtmp_passcode ? " (stream key set)" : "");
            }
            g_free(location);
        } else {
            LOG_E("RTMP sink requested without rtmp_uri");
        }
    } else if (strcmp(sink_type, "file") == 0) {
        char *location = resolve_file_location(config);
        if (location && strlen(location) > 0) {
            sink = gst_element_factory_make("filesink", NULL);
            if (sink) {
                g_object_set(sink, "location", location, "sync", FALSE, NULL);
                LOG_I("File sink: %s", location);
            }
        } else {
            LOG_E("file sink requested without file_path");
        }
        g_free(location);
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
    muxer = create_muxer(mgr->codec, "srt", NULL, &muxer_format);

    sink = gst_element_factory_make("appsink", NULL);

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
        NULL);
    set_appsrc_queue_limits(video_src,
                            (guint64)(mgr->bitrate_kbps * 1000 / 2), 8u);
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
        "max-bytes",    SBS_SRT_AUDIO_APP_MAX_BYTES,
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
        "max-size-buffers", (guint)0,
        "max-size-time",    (guint64)SBS_SRT_AUDIO_BUFFER_NS,
        "max-size-bytes",   (guint)0,
        "leaky",            0,
        NULL);
    g_object_set(video_pacer,
        "sync", FALSE,
        NULL);
    g_object_set(audio_encoder, "bitrate", 192000, NULL);
    g_object_set(parser,
        "config-interval",    (gint)-1,
        "disable-passthrough", TRUE,
        NULL);
    g_object_set(sink,
        "emit-signals", TRUE,
        "sync",         FALSE,
        "async",        FALSE,
        "max-buffers",  (guint)256,
        "drop",         TRUE,
        NULL);

    branch->manager = mgr;
    branch->drop_until_keyframe = TRUE;
    branch->srt_caller_count = 0;
    g_signal_connect(sink, "new-sample",
                     G_CALLBACK(on_srt_ts_sample), branch);

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

    int srt_rc = start_custom_srt_sender(branch);
    if (srt_rc != SBS_OK) {
        LOG_E("SRT session '%s' failed to start custom sender", branch->output_id);
        bus = gst_element_get_bus(pipeline);
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return srt_rc;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_E("SRT session '%s' failed to enter PLAYING", branch->output_id);
        bus = gst_element_get_bus(pipeline);
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        stop_custom_srt_sender(branch);
        gst_object_unref(pipeline);
        return SBS_ERR_IO;
    }

    branch->srt_pipeline = pipeline;
    branch->srt_video_appsrc = gst_object_ref(video_src);
    branch->srt_audio_appsrc = gst_object_ref(audio_src);
    branch->audio_appsrc = branch->srt_audio_appsrc;
    branch->muxer = muxer;
    branch->sink = sink;
    branch->direct_audio = true;
    if (!branch->srt_callers)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);
    else
        g_hash_table_remove_all(branch->srt_callers);
    branch->srt_video_base_valid = false;
    branch->srt_video_base_pts_ns = 0;
    branch->srt_video_base_dts_ns = 0;
    branch->srt_video_running_origin_ns = 0;
    branch->srt_video_max_pts_ns = 0;
    branch->srt_video_buffers_pushed = 0;
    branch->srt_video_push_failures = 0;
    branch->srt_stats_bytes_valid = false;
    branch->srt_last_bytes_sent_total = 0;
    branch->srt_last_bytes_change_video_count = 0;

    if (!branch->mux_feed_initialized) {
        pthread_mutex_init(&branch->mux_feed_mutex, NULL);
        pthread_cond_init(&branch->mux_feed_cond, NULL);
        branch->mux_video_queue = g_queue_new();
        branch->mux_audio_queue = g_queue_new();
        branch->mux_feed_initialized = true;
    }
    pthread_mutex_lock(&branch->mux_feed_mutex);
    mux_feed_reset_locked(branch);
    branch->mux_feed_running = true;
    branch->mux_feed_started = false;
    pthread_mutex_unlock(&branch->mux_feed_mutex);
    if (pthread_create(&branch->mux_feed_thread, NULL, mux_feed_thread_main, branch) != 0) {
        LOG_E("failed to start SRT mux feeder for '%s'", branch->output_id);
        pthread_mutex_lock(&branch->mux_feed_mutex);
        branch->mux_feed_running = false;
        pthread_mutex_unlock(&branch->mux_feed_mutex);
        stop_srt_session(mgr, branch);
        return SBS_ERR_IO;
    }
    branch->mux_feed_started = true;

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

    if (branch->mux_feed_initialized) {
        pthread_mutex_lock(&branch->mux_feed_mutex);
        branch->mux_feed_running = false;
        pthread_cond_signal(&branch->mux_feed_cond);
        pthread_mutex_unlock(&branch->mux_feed_mutex);
        if (branch->mux_feed_started) {
            pthread_join(branch->mux_feed_thread, NULL);
            branch->mux_feed_started = false;
        }
        pthread_mutex_lock(&branch->mux_feed_mutex);
        mux_feed_reset_locked(branch);
        pthread_mutex_unlock(&branch->mux_feed_mutex);
    }

    stop_custom_srt_sender(branch);

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
    GstElement *vparser = NULL;
    GstElement *vcapsfilter = NULL;
    GstElement *vpacer = gst_element_factory_make("identity", NULL);
    GstElement *aqueue = gst_element_factory_make("queue", NULL);
    const char *muxer_format = NULL;
    GstElement *muxer  = create_muxer(mgr->codec, branch->sink_type, branch->file_container, &muxer_format);
    GstElement *acapsfilter = NULL;
    GstElement *branch_audio_src = NULL;
    GstElement *branch_audio_convert = NULL;
    GstElement *branch_audio_resample = NULL;
    GstElement *branch_audio_encoder = NULL;
    GstElement *branch_audio_parser = NULL;
    bool direct_audio = branch->sink_type && strcmp(branch->sink_type, "srt") == 0;
    bool flv_muxer = muxer_format && strcmp(muxer_format, "flv") == 0;

    sbs_sink_branch_config_t cfg = {
        .output_id    = branch->output_id,
        .sink_type    = branch->sink_type,
        .srt_uri      = branch->srt_uri,
        .srt_mode     = branch->srt_mode,
        .srt_stream_key = branch->srt_stream_key,
        .srt_passphrase = branch->srt_passphrase,
        .srt_latency_ms = branch->srt_latency_ms,
        .rtmp_uri     = branch->rtmp_uri,
        .rtmp_passcode = branch->rtmp_passcode,
        .rtmp_flv_mode = branch->rtmp_flv_mode,
        .file_path    = branch->file_path,
        .file_path_mode = branch->file_path_mode,
        .file_prefix  = branch->file_prefix,
        .file_container = branch->file_container,
    };
    GstElement *sink = create_sink(&cfg, muxer_format);

    if (sink && branch->sink_type && strcmp(branch->sink_type, "rtmp") == 0 &&
        (mgr->is_h265 || mgr->hdr10)) {
        const char *enhanced_codecs = mgr->is_h265 ? "hvc1" : "avc1";
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "enhanced-codecs")) {
            g_object_set(sink, "enhanced-codecs", enhanced_codecs, NULL);
            LOG_I("RTMP Enhanced codec advertise enabled for '%s': %s",
                  branch->output_id ? branch->output_id : "?",
                  enhanced_codecs);
        }
    }

    if (flv_muxer) {
        GstCaps *vcaps;
        vparser = gst_element_factory_make(mgr->is_h265 ? "h265parse" : "h264parse", NULL);
        vcapsfilter = gst_element_factory_make("capsfilter", NULL);
        if (vparser) {
            g_object_set(vparser,
                "config-interval",     (gint)-1,
                "disable-passthrough", TRUE,
                NULL);
        }
        if (mgr->is_h265) {
            vcaps = gst_caps_new_simple("video/x-h265",
                "stream-format", G_TYPE_STRING, "hvc1",
                "alignment",     G_TYPE_STRING, "au",
                NULL);
        } else {
            vcaps = gst_caps_new_simple("video/x-h264",
                "stream-format", G_TYPE_STRING, "avc",
                "alignment",     G_TYPE_STRING, "au",
                NULL);
        }
        if (vcapsfilter)
            g_object_set(vcapsfilter, "caps", vcaps, NULL);
        gst_caps_unref(vcaps);
    }

    branch->manager = mgr;
    branch->drop_until_keyframe = TRUE;
    branch->srt_caller_count = 0;
    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 && !branch->srt_callers)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);

    if (muxer_format &&
        (strcmp(muxer_format, "mpegts") == 0 || strcmp(muxer_format, "flv") == 0)) {
        GstCaps *caps;
        acapsfilter = gst_element_factory_make("capsfilter", NULL);
        if (strcmp(muxer_format, "flv") == 0) {
            caps = gst_caps_new_simple("audio/mpeg",
                "mpegversion",   G_TYPE_INT, 4,
                "stream-format", G_TYPE_STRING, "raw",
                NULL);
        } else if (direct_audio) {
            caps = gst_caps_new_simple("audio/mpeg",
                "parsed",      G_TYPE_BOOLEAN, TRUE,
                "mpegversion", G_TYPE_INT, 1,
                "layer",       G_TYPE_INT, 2,
                NULL);
        } else {
            caps = gst_caps_new_simple("audio/mpeg",
                "framed",        G_TYPE_BOOLEAN, TRUE,
                "mpegversion",   G_TYPE_INT, 4,
                "stream-format", G_TYPE_STRING, "raw",
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
        (flv_muxer && (!vparser || !vcapsfilter)) ||
        (muxer_format && strcmp(muxer_format, "mpegts") == 0 && !acapsfilter) ||
        (direct_audio && (!branch_audio_src || !branch_audio_convert ||
                          !branch_audio_resample || !branch_audio_encoder ||
                          !branch_audio_parser))) {
        LOG_E("failed to create sink branch elements for '%s'", branch->output_id);
        if (vqueue) gst_object_unref(vqueue);
        if (vparser) gst_object_unref(vparser);
        if (vcapsfilter) gst_object_unref(vcapsfilter);
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
            "max-bytes",    SBS_SRT_AUDIO_APP_MAX_BYTES,
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

    if (branch->sink_type && strcmp(branch->sink_type, "rtmp") == 0) {
        GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
        if (sink_pad) {
            branch->sink_probe_id = gst_pad_add_probe(
                sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                branch_sink_buffer_probe, branch, NULL);
            gst_object_unref(sink_pad);
        }
    }

    if (branch->sink_type && strcmp(branch->sink_type, "rtmp") == 0 &&
        flv_muxer && (mgr->is_h265 || mgr->hdr10)) {
        GstPad *mux_src_pad = gst_element_get_static_pad(muxer, "src");
        if (mux_src_pad) {
            branch->flv_hevc_pending = g_byte_array_new();
            branch->flv_hevc_probe_id = gst_pad_add_probe(
                mux_src_pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                flv_hevc_probe, branch, NULL);
            gst_object_unref(mux_src_pad);
            LOG_I("RTMP FLV adapter enabled for '%s' (codec=%s mode=%s hdr10=%d)",
                  branch->output_id ? branch->output_id : "?",
                  mgr->is_h265 ? "h265" : "h264",
                  branch->rtmp_flv_mode ? branch->rtmp_flv_mode : "enhanced",
                  mgr->hdr10 ? 1 : 0);
        }
    }

    if (branch->sink_type &&
        (strcmp(branch->sink_type, "srt") == 0 ||
         strcmp(branch->sink_type, "rtmp") == 0)) {
        g_object_set(vqueue,
            "max-size-buffers", (guint)8,
            "max-size-time",    (guint64)(250 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    } else {
        g_object_set(vqueue,
            "max-size-buffers", (guint)0,
            "max-size-time",    (guint64)(2 * GST_SECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            0,
            NULL);
    }

    if (direct_audio) {
        g_object_set(aqueue,
            "max-size-buffers", (guint)0,
            "max-size-time",    (guint64)SBS_SRT_AUDIO_BUFFER_NS,
            "max-size-bytes",   (guint)0,
            "leaky",            0,
            NULL);
    } else if (branch->sink_type && strcmp(branch->sink_type, "rtmp") == 0) {
        g_object_set(aqueue,
            "max-size-buffers", (guint)0,
            "max-size-time",    (guint64)(250 * GST_MSECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            2,
            NULL);
    } else {
        g_object_set(aqueue,
            "max-size-buffers", (guint)0,
            "max-size-time",    (guint64)(2 * GST_SECOND),
            "max-size-bytes",   (guint)0,
            "leaky",            0,
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
    } else if (vparser || vcapsfilter) {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vparser, vcapsfilter,
                         vpacer, aqueue, acapsfilter, muxer, sink, NULL);
    } else if (acapsfilter) {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue,
                         acapsfilter, muxer, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue,
                         muxer, sink, NULL);
    }

    /* Link: vqueue → timestamp pacer → muxer → sink */
    gboolean video_linked;
    if (vparser && vcapsfilter)
        video_linked = gst_element_link_many(vqueue, vparser, vcapsfilter, vpacer, muxer, sink, NULL);
    else
        video_linked = gst_element_link_many(vqueue, vpacer, muxer, sink, NULL);
    if (!video_linked) {
        LOG_E("failed to link video branch for '%s'", branch->output_id);
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
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
            LOG_E("failed to link audio branch into muxer for '%s'", branch->output_id);
            if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
            if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
            return SBS_ERR_IO;
        }
    } else if (!gst_element_link(aqueue, muxer)) {
        LOG_E("failed to link audio branch into muxer for '%s'", branch->output_id);
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
        gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }

    /* Request pad from video tee and link */
    GstPad *vtee_pad = gst_element_request_pad_simple(mgr->video_tee, "src_%u");
    GstPad *vq_sink  = gst_element_get_static_pad(vqueue, "sink");
    if (!vtee_pad || !vq_sink) {
        LOG_E("failed to request video branch pads for '%s'", branch->output_id);
        if (vtee_pad) gst_object_unref(vtee_pad);
        if (vq_sink) gst_object_unref(vq_sink);
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    GstPadLinkReturn vlink_ret = gst_pad_link(vtee_pad, vq_sink);
    if (vlink_ret != GST_PAD_LINK_OK) {
        LOG_E("failed to link video tee to queue for '%s': %s",
              branch->output_id, gst_pad_link_get_name(vlink_ret));
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        gst_object_unref(vq_sink);
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
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
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    GstPadLinkReturn alink_ret = direct_audio ? GST_PAD_LINK_OK : gst_pad_link(atee_pad, aq_sink);
    if (!direct_audio && alink_ret != GST_PAD_LINK_OK) {
        GstPad *vq_sink2 = gst_element_get_static_pad(vqueue, "sink");
        LOG_E("failed to link audio tee to queue for '%s': %s",
              branch->output_id, gst_pad_link_get_name(alink_ret));
        if (vq_sink2) {
            gst_pad_unlink(vtee_pad, vq_sink2);
            gst_object_unref(vq_sink2);
        }
        gst_element_release_request_pad(mgr->video_tee, vtee_pad);
        gst_object_unref(vtee_pad);
        gst_element_release_request_pad(mgr->audio_tee, atee_pad);
        gst_object_unref(atee_pad);
        gst_object_unref(aq_sink);
        if (vcapsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), vcapsfilter);
        if (vparser) gst_bin_remove(GST_BIN(mgr->pipeline), vparser);
        if (acapsfilter)
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, acapsfilter, muxer, sink, NULL);
        else
            gst_bin_remove_many(GST_BIN(mgr->pipeline), vqueue, vpacer, aqueue, muxer, sink, NULL);
        return SBS_ERR_IO;
    }
    gst_object_unref(aq_sink);

    /* Sync branch state only after tee pads are linked. Some live sinks try to
     * preroll/connect as soon as they enter PLAYING, which can make dynamic pad
     * linking fail if the branch is not connected yet. */
    gst_element_sync_state_with_parent(vqueue);
    if (vparser) gst_element_sync_state_with_parent(vparser);
    if (vcapsfilter) gst_element_sync_state_with_parent(vcapsfilter);
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

    /* Store references in branch. */
    branch->video_queue    = vqueue;
    branch->video_parser   = vparser;
    branch->video_capsfilter = vcapsfilter;
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
    if (branch->sink && branch->sink_probe_id != 0) {
        GstPad *sink_pad = gst_element_get_static_pad(branch->sink, "sink");
        if (sink_pad) {
            gst_pad_remove_probe(sink_pad, branch->sink_probe_id);
            gst_object_unref(sink_pad);
        }
        branch->sink_probe_id = 0;
    }
    if (branch->muxer && branch->flv_hevc_probe_id != 0) {
        GstPad *mux_src_pad = gst_element_get_static_pad(branch->muxer, "src");
        if (mux_src_pad) {
            gst_pad_remove_probe(mux_src_pad, branch->flv_hevc_probe_id);
            gst_object_unref(mux_src_pad);
        }
        branch->flv_hevc_probe_id = 0;
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
    if (branch->video_capsfilter) {
        LOG_I("unlink branch '%s': set video capsfilter NULL", branch->output_id);
        gst_element_set_state(branch->video_capsfilter, GST_STATE_NULL);
    }
    if (branch->video_parser) {
        LOG_I("unlink branch '%s': set video parser NULL", branch->output_id);
        gst_element_set_state(branch->video_parser, GST_STATE_NULL);
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
        branch->video_parser = NULL;
        branch->video_capsfilter = NULL;
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
    if (branch->video_capsfilter) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_capsfilter);
    if (branch->video_parser) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_parser);
    if (branch->video_pacer) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_pacer);
    if (branch->video_queue) gst_bin_remove(GST_BIN(mgr->pipeline), branch->video_queue);
    LOG_I("unlink branch '%s': remove complete", branch->output_id);

    branch->video_queue = NULL;
    branch->video_parser = NULL;
    branch->video_capsfilter = NULL;
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
    mgr->bitrate_kbps     = enc_config->bitrate_kbps > 0 ? enc_config->bitrate_kbps : 10000;
    mgr->gop_size         = enc_config->gop_size;
    mgr->gop_pattern      = enc_config->gop_pattern;
    mgr->rc_mode          = enc_config->rc_mode;
    mgr->encoder_override = g_strdup(enc_config->encoder);
    mgr->pixel_format     = enc_config->pixel_format;
    mgr->colorimetry      = enc_config->colorimetry;
    if (enc_config->hdr10 &&
        mgr->pixel_format == SBS_PIXEL_FORMAT_NV21 &&
        mgr->colorimetry == SBS_COLORIMETRY_SDR) {
        mgr->pixel_format = SBS_PIXEL_FORMAT_P010;
        mgr->colorimetry = SBS_COLORIMETRY_BT2020_PQ;
    }
    mgr->hdr10            = mgr->colorimetry == SBS_COLORIMETRY_BT2020_PQ;

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

    LOG_I("encoder manager created: %ux%u@%u/%u codec=%s bitrate=%u pixel_format=%s colorimetry=%s",
          width, height, fps_num, mgr->fps_den,
          mgr->codec, mgr->bitrate_kbps,
          sbs_pixel_format_name(mgr->pixel_format),
          sbs_colorimetry_name(mgr->colorimetry));

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
    uint32_t new_bitrate = enc_config->bitrate_kbps > 0 ? enc_config->bitrate_kbps : 10000;

    bool codec_changed = (strcmp(mgr->codec, new_codec) != 0);
    bool bitrate_changed = (mgr->bitrate_kbps != new_bitrate);
    bool gop_changed = (mgr->gop_size != enc_config->gop_size);
    bool gop_pattern_changed = (mgr->gop_pattern != enc_config->gop_pattern);
    bool rc_mode_changed = (mgr->rc_mode != enc_config->rc_mode);
    sbs_pixel_format_t new_pixel_format = enc_config->pixel_format;
    sbs_colorimetry_t new_colorimetry = enc_config->colorimetry;
    if (enc_config->hdr10 &&
        new_pixel_format == SBS_PIXEL_FORMAT_NV21 &&
        new_colorimetry == SBS_COLORIMETRY_SDR) {
        new_pixel_format = SBS_PIXEL_FORMAT_P010;
        new_colorimetry = SBS_COLORIMETRY_BT2020_PQ;
    }
    bool pixel_format_changed = (mgr->pixel_format != new_pixel_format);
    bool colorimetry_changed = (mgr->colorimetry != new_colorimetry);

    if (!codec_changed && !bitrate_changed && !gop_changed && !gop_pattern_changed &&
        !rc_mode_changed && !pixel_format_changed && !colorimetry_changed) {
        LOG_D("encoder config unchanged, skipping rebuild");
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return SBS_OK;
    }

    LOG_I("encoder config changed: codec=%s→%s bitrate=%u→%u gop=%u→%u gop_pattern=%d→%d rc_mode=%d→%d pixel_format=%s→%s colorimetry=%s→%s",
          mgr->codec, new_codec, mgr->bitrate_kbps, new_bitrate,
          mgr->gop_size, enc_config->gop_size, mgr->gop_pattern, enc_config->gop_pattern,
          mgr->rc_mode, enc_config->rc_mode,
          sbs_pixel_format_name(mgr->pixel_format),
          sbs_pixel_format_name(new_pixel_format),
          sbs_colorimetry_name(mgr->colorimetry),
          sbs_colorimetry_name(new_colorimetry));

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
        copy->srt_mode     = g_strdup(branch->srt_mode);
        copy->srt_stream_key = g_strdup(branch->srt_stream_key);
        copy->srt_passphrase = g_strdup(branch->srt_passphrase);
        copy->srt_latency_ms = branch->srt_latency_ms;
        copy->rtmp_uri     = g_strdup(branch->rtmp_uri);
        copy->rtmp_passcode = g_strdup(branch->rtmp_passcode);
        copy->rtmp_flv_mode = g_strdup(branch->rtmp_flv_mode);
        copy->file_path    = g_strdup(branch->file_path);
        copy->file_path_mode = g_strdup(branch->file_path_mode);
        copy->file_prefix  = g_strdup(branch->file_prefix);
        copy->file_container = g_strdup(branch->file_container);
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
    mgr->pixel_format = new_pixel_format;
    mgr->colorimetry = new_colorimetry;
    mgr->hdr10 = mgr->colorimetry == SBS_COLORIMETRY_BT2020_PQ;

    /* Teardown and rebuild */
    LOG_I("encoder teardown starting...");
    teardown_pipeline(mgr);
    LOG_I("encoder teardown done, rebuilding with pixel_format=%s colorimetry=%s...",
          sbs_pixel_format_name(mgr->pixel_format),
          sbs_colorimetry_name(mgr->colorimetry));

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
            .srt_mode     = saved->srt_mode,
            .srt_stream_key = saved->srt_stream_key,
            .srt_passphrase = saved->srt_passphrase,
            .srt_latency_ms = saved->srt_latency_ms,
            .rtmp_uri     = saved->rtmp_uri,
            .rtmp_passcode = saved->rtmp_passcode,
            .rtmp_flv_mode = saved->rtmp_flv_mode,
            .file_path    = saved->file_path,
            .file_path_mode = saved->file_path_mode,
            .file_prefix  = saved->file_prefix,
            .file_container = saved->file_container,
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

int sbs_encoder_manager_reconfigure_video(sbs_encoder_manager_t *mgr,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t fps_num,
                                           uint32_t fps_den,
                                           sbs_pixel_format_t pixel_format,
                                           sbs_colorimetry_t colorimetry)
{
    uint32_t old_fps_num;

    if (!mgr || width == 0 || height == 0 || fps_num == 0)
        return SBS_ERR_INVAL;
    if (fps_den == 0)
        fps_den = 1;

    pthread_mutex_lock(&mgr->pipeline_mutex);
    if (mgr->width == width && mgr->height == height &&
        mgr->fps_num == fps_num && mgr->fps_den == fps_den &&
        mgr->pixel_format == pixel_format &&
        mgr->colorimetry == colorimetry) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return SBS_OK;
    }

    LOG_I("reconfiguring encoder video: %ux%u@%u/%u %s/%s -> %ux%u@%u/%u %s/%s",
          mgr->width, mgr->height, mgr->fps_num, mgr->fps_den,
          sbs_pixel_format_name(mgr->pixel_format),
          sbs_colorimetry_name(mgr->colorimetry),
          width, height, fps_num, fps_den,
          sbs_pixel_format_name(pixel_format),
          sbs_colorimetry_name(colorimetry));

    mgr->pipeline_active = false;
    teardown_pipeline(mgr);

    old_fps_num = mgr->fps_num;
    mgr->width = width;
    mgr->height = height;
    mgr->fps_num = fps_num;
    mgr->fps_den = fps_den;
    mgr->pixel_format = pixel_format;
    mgr->colorimetry = colorimetry;
    mgr->hdr10 = mgr->colorimetry == SBS_COLORIMETRY_BT2020_PQ;
    if (mgr->gop_size == old_fps_num)
        mgr->gop_size = fps_num;
    mgr->force_next_idr = true;

    {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, mgr->branches);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            flv_hevc_cache_hdr_metadata_values(value, mgr);
    }

    pthread_mutex_unlock(&mgr->pipeline_mutex);
    return SBS_OK;
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
    if (rc == SBS_OK || rc == SBS_ERR_WOULD_BLOCK)
        mgr->force_next_idr = false;
    if (rc == SBS_ERR_WOULD_BLOCK) {
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }
    if (rc != SBS_OK) {
        mgr->frames_dropped++;
        pthread_mutex_unlock(&mgr->pipeline_mutex);
        return;
    }
    if (!packet.data || packet.size == 0) {
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
    if (mgr->direct_audio_branches) {
        for (guint i = 0; i < mgr->direct_audio_branches->len; i++) {
            sink_branch_t *branch = g_ptr_array_index(mgr->direct_audio_branches, i);
            GstClockTime branch_pts;
            GstBuffer *branch_buffer;

            if (!branch->direct_audio || !branch->audio_appsrc)
                continue;
            if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0 &&
                (g_atomic_int_get(&branch->srt_caller_count) <= 0 ||
                 g_atomic_int_get(&branch->drop_until_keyframe)))
                continue;
            branch_pts = direct_audio_branch_next_pts(branch, pts_ns, duration_ns);
            branch_buffer = make_audio_buffer(msg, audio_data, branch_pts, duration_ns);
            if (branch_buffer)
                mux_feed_enqueue_buffer(branch, branch_buffer, false);
        }
    }
    pthread_mutex_unlock(&mgr->audio_mutex);

    GstBuffer *buffer = make_audio_buffer(msg, audio_data, pts_ns, duration_ns);
    if (buffer)
        ret = gst_app_src_push_buffer(GST_APP_SRC(audio_appsrc), buffer);
    else
        ret = GST_FLOW_ERROR;
    gst_object_unref(audio_appsrc);

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
    branch->srt_mode     = g_strdup(config->srt_mode);
    branch->srt_stream_key = g_strdup(config->srt_stream_key);
    branch->srt_passphrase = g_strdup(config->srt_passphrase);
    branch->srt_latency_ms = config->srt_latency_ms;
    if (branch->sink_type && strcmp(branch->sink_type, "srt") == 0)
        branch->srt_callers = g_hash_table_new(g_direct_hash, g_direct_equal);
    branch->rtmp_uri     = g_strdup(config->rtmp_uri);
    branch->rtmp_passcode = g_strdup(config->rtmp_passcode);
    branch->rtmp_flv_mode = g_strdup(normalized_rtmp_flv_mode(config->rtmp_flv_mode));
    branch->flv_hevc_legacy_mode = strcmp(branch->rtmp_flv_mode, "legacy") == 0;
    flv_hevc_cache_hdr_metadata_values(branch, mgr);
    branch->file_path    = g_strdup(config->file_path);
    branch->file_path_mode = g_strdup(config->file_path_mode ? config->file_path_mode : "file");
    branch->file_prefix  = g_strdup(config->file_prefix ? config->file_prefix : "stream");
    branch->file_container = g_strdup(normalized_file_container(config->file_container));
    pthread_mutex_init(&branch->sink_stats_mutex, NULL);
    branch->sink_stats_initialized = true;

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

static cJSON *serialize_branch_health(sink_branch_t *branch)
{
    cJSON *obj = cJSON_CreateObject();
    const char *sink_type = branch && branch->sink_type ? branch->sink_type : "srt";
    const char *status = "disconnected";
    const char *reason = "not connected";
    bool connected = false;
    bool degraded = false;

    int warnings = branch ? g_atomic_int_get((gint *)&branch->sink_warning_count) : 0;
    int errors = branch ? g_atomic_int_get((gint *)&branch->sink_error_count) : 0;
    int sink_buffers = 0;
    uint64_t sink_bytes = 0;
    int srt_callers = branch ? g_atomic_int_get((gint *)&branch->srt_caller_count) : 0;
    uint64_t srt_failures = branch
        ? branch->srt_sender_send_failures + branch->srt_video_push_failures +
          branch->direct_audio_push_failures
        : 0;
    uint64_t packets_sent = 0;
    uint64_t packets_dropped = 0;
    bool srt_caller_mode = branch && srt_mode_is_caller(branch->srt_mode);

    if (branch) {
        if (branch->sink_stats_initialized)
            pthread_mutex_lock(&branch->sink_stats_mutex);
        sink_buffers = branch->sink_buffers_seen;
        sink_bytes = branch->sink_bytes_seen;
        if (branch->sink_stats_initialized)
            pthread_mutex_unlock(&branch->sink_stats_mutex);
    }

    if (g_strcmp0(sink_type, "srt") == 0) {
        packets_sent = branch ? branch->srt_sender_packets_sent : 0;
        packets_dropped = branch ? branch->srt_sender_packets_dropped : 0;
        if (!branch || !branch->srt_sender_running) {
            reason = srt_caller_mode ? "SRT caller is not connected" : "SRT listener is not running";
        } else if (srt_callers <= 0) {
            reason = srt_caller_mode ? "SRT caller disconnected" : "waiting for SRT caller";
        } else if (srt_failures > 0) {
            status = "degraded";
            connected = true;
            degraded = true;
            reason = "SRT send failures detected";
        } else if (branch->srt_sender_bytes_sent > 0) {
            status = "connected";
            connected = true;
            reason = "SRT stream active";
        } else {
            status = "degraded";
            connected = true;
            degraded = true;
            reason = "SRT caller connected, waiting for packets";
        }
    } else if (g_strcmp0(sink_type, "rtmp") == 0) {
        packets_sent = (uint64_t)MAX(sink_buffers, 0);
        if (!branch || !branch->rtmp_uri || !*branch->rtmp_uri) {
            reason = "RTMP destination not configured";
        } else if (sink_buffers > 0 && errors > 0) {
            status = "degraded";
            connected = true;
            degraded = true;
            reason = "RTMP sink errors detected";
        } else if (errors > 0) {
            reason = "RTMP sink error before traffic";
        } else if (warnings > 0 && sink_buffers > 0) {
            status = "degraded";
            connected = true;
            degraded = true;
            reason = "RTMP sink warnings detected";
        } else if (sink_buffers > 0) {
            status = "connected";
            connected = true;
            reason = "RTMP sink streaming";
        } else {
            status = "degraded";
            connected = true;
            degraded = true;
            reason = "RTMP sink active, waiting for traffic";
        }
    } else if (branch) {
        status = "connected";
        connected = true;
        reason = "output branch active";
    }

    cJSON_AddStringToObject(obj, "sink_type", sink_type);
    cJSON_AddStringToObject(obj, "status", status);
    cJSON_AddBoolToObject(obj, "connected", connected);
    cJSON_AddBoolToObject(obj, "degraded", degraded);
    cJSON_AddStringToObject(obj, "reason", reason);
    cJSON_AddNumberToObject(obj, "warnings", warnings);
    cJSON_AddNumberToObject(obj, "errors", errors);
    cJSON_AddNumberToObject(obj, "sink_buffers", sink_buffers);
    cJSON_AddNumberToObject(obj, "sink_bytes", (double)sink_bytes);
    cJSON_AddNumberToObject(obj, "packets_sent", (double)packets_sent);
    cJSON_AddNumberToObject(obj, "packets_dropped", (double)packets_dropped);
    cJSON_AddNumberToObject(obj, "packet_drop_rate",
                            packets_sent + packets_dropped > 0
                                ? (double)packets_dropped / (double)(packets_sent + packets_dropped)
                                : 0.0);
    if (g_strcmp0(sink_type, "srt") == 0) {
        cJSON_AddStringToObject(obj, "mode", srt_caller_mode ? "caller" : "listener");
        cJSON_AddNumberToObject(obj, "callers", srt_callers);
        cJSON_AddNumberToObject(obj, "bytes_sent", (double)(branch ? branch->srt_sender_bytes_sent : 0));
        cJSON_AddNumberToObject(obj, "send_failures", (double)srt_failures);
    } else if (g_strcmp0(sink_type, "rtmp") == 0) {
        cJSON_AddNumberToObject(obj, "bytes_sent", (double)sink_bytes);
    }
    return obj;
}

cJSON *sbs_encoder_manager_serialize_output_health(const sbs_encoder_manager_t *mgr)
{
    cJSON *root = cJSON_CreateObject();
    sbs_encoder_manager_t *mutable_mgr;

    if (!mgr)
        return root;

    mutable_mgr = (sbs_encoder_manager_t *)mgr;
    pthread_mutex_lock(&mutable_mgr->pipeline_mutex);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mutable_mgr->branches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sink_branch_t *branch = value;
        const char *output_id = branch && branch->output_id ? branch->output_id : key;
        if (output_id)
            cJSON_AddItemToObject(root, output_id, serialize_branch_health(branch));
    }
    pthread_mutex_unlock(&mutable_mgr->pipeline_mutex);
    return root;
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
    out_config->pixel_format = mgr->pixel_format;
    out_config->colorimetry  = mgr->colorimetry;
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
