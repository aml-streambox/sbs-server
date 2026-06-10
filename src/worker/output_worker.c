/*
 * SBS - StreamBox Broadcast System
 * Output worker — receives composed frames via IPC, encodes and streams via SRT
 *
 * Pipeline: appsrc ! [encoder] ! [parser] ! mpegtsmux ! srtsink
 *
 * For target (A311D2): appsrc ! amlvenc ! h265parse ! mpegtsmux ! srtsink
 * For host fallback:   appsrc ! x264enc/x265enc ! parser ! mpegtsmux ! srtsink
 *
 * References: document/06-output-manager.md sections 5-7
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "output"

#include "worker_common.h"
#include "sbs/log.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <glib-unix.h>

#include <gst/app/gstappsrc.h>
#include <gst/video/video-info.h>

/* ── Output Worker State ──────────────────────────────────────── */

typedef struct output_state {
    sbs_worker_ctx_t  *ctx;
    GstElement        *pipeline;
    GstElement        *appsrc;
    GstElement        *audio_appsrc;
    uint64_t           frames_consumed;
    uint64_t           audio_buffers_consumed;
    uint64_t           frames_encoded;
    uint64_t           frames_dropped;
    uint64_t           bytes_written;
    guint              heartbeat_timer;
    guint              io_watch_id;
} output_state_t;

/* ── Forward Declarations ─────────────────────────────────────── */

static GstElement *build_output_pipeline(sbs_worker_config_t *config);
static gboolean    on_frame_received(gint fd, GIOCondition cond, gpointer user_data);
static gboolean    on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data);
static gboolean    on_heartbeat(gpointer user_data);

/* ── Encoder Element Selection ────────────────────────────────── */

/**
 * Try to create an encoder element by name.
 * Returns the element on success, or NULL if not in registry.
 */
static GstElement *try_create_encoder(const char *factory_name, const char *elem_name)
{
    GstElementFactory *factory = gst_element_factory_find(factory_name);
    if (!factory) return NULL;

    GstElement *elem = gst_element_factory_create(factory, elem_name);
    gst_object_unref(factory);
    return elem;
}

/**
 * Select and create the appropriate encoder element.
 *
 * Priority for H.265: amlvenc → x265enc
 * Priority for H.264: amlvenc → x264enc
 *
 * Returns the encoder element, or NULL if none available.
 * Sets *parser_name to the appropriate parser element name.
 */
static GstElement *select_encoder(const char *codec, uint32_t bitrate,
                                   const char *override_encoder,
                                   const char **parser_name)
{
    GstElement *encoder = NULL;
    bool is_h265 = (codec && strcmp(codec, "h265") == 0);

    *parser_name = is_h265 ? "h265parse" : "h264parse";

    /* If user specified an encoder override, use that */
    if (override_encoder && strlen(override_encoder) > 0) {
        encoder = try_create_encoder(override_encoder, "encoder");
        if (encoder) {
            LOG_I("using override encoder: %s", override_encoder);
            return encoder;
        }
        LOG_W("override encoder '%s' not found, falling back", override_encoder);
    }

    /* Try hardware encoder first */
    encoder = try_create_encoder("amlvenc", "encoder");
    if (encoder) {
        LOG_I("using hardware encoder: amlvenc (codec=%s)", codec ? codec : "h265");
        /* amlvenc uses downstream caps for codec selection.
         * Keep enable-dmallocator=true (default) so amlvenc can internally
         * allocate DMA buffers for the hardware encoder when receiving
         * CPU-only input buffers.
         * Do NOT set gop/framerate explicitly — amlvenc infers them from
         * the negotiated caps and clock. Setting them manually causes
         * encoder initialization issues (all TRAIL_R, no VPS/SPS/PPS).
         * Bitrate property is in kbps (config value is already kbps). */
        if (bitrate > 0) {
            g_object_set(encoder, "bitrate", (gint)bitrate, NULL);
        }
        return encoder;
    }

    /* Fallback to software encoder */
    if (is_h265) {
        encoder = try_create_encoder("x265enc", "encoder");
        if (encoder) {
            LOG_I("using software encoder: x265enc");
            if (bitrate > 0) {
                g_object_set(encoder, "bitrate", (guint)bitrate, NULL);
            }
            /* x265enc speed preset for real-time */
            g_object_set(encoder, "speed-preset", 1 /* ultrafast */, NULL);
            return encoder;
        }
    } else {
        encoder = try_create_encoder("x264enc", "encoder");
        if (encoder) {
            LOG_I("using software encoder: x264enc");
            if (bitrate > 0) {
                g_object_set(encoder, "bitrate", (guint)bitrate, NULL);
            }
            /* x264enc speed preset for real-time */
            g_object_set(encoder, "speed-preset", 1 /* ultrafast */,
                         "tune", 4 /* zerolatency */, NULL);
            return encoder;
        }
    }

    LOG_E("no suitable encoder found for codec '%s'", codec ? codec : "(null)");
    return NULL;
}

/* ── H.265 Stream Sanitizer (pad probe) ──────────────────────── */

/**
 * Pad probe on h265parse src pad.
 *
 * amlvenc sometimes emits 1-2 TRAIL_R NAL units before the first
 * VPS/SPS/PPS/IDR when fed from appsrc (does NOT happen with videotestsrc
 * directly linked). These leading garbage NALs cause decoders to fail with
 * "PPS id out of range: 0" since no parameter sets have been seen yet.
 *
 * This probe inspects each buffer leaving h265parse and drops everything
 * until we see a buffer that starts with VPS (NAL type 32). Once the first
 * VPS is found, the probe removes itself — all subsequent buffers pass
 * through unmodified.
 *
 * H.265 NAL header: forbidden_zero_bit(1) + nal_unit_type(6) + nuh_layer_id(6) + nuh_temporal_id_plus1(3)
 * nal_unit_type = (first_byte >> 1) & 0x3F
 */
static GstPadProbeReturn
h265_stream_sanitize_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)user_data;

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return GST_PAD_PROBE_DROP;

    /* Scan for a VPS NAL unit (type 32).
     * h265parse outputs in byte-stream format with 00 00 00 01 start codes.
     * We look for the start code and check the NAL type. */
    gboolean found_vps = FALSE;

    for (gsize i = 0; i + 4 < map.size; i++) {
        /* Look for 3-byte or 4-byte start code */
        if ((map.data[i] == 0x00 && map.data[i+1] == 0x00 &&
             map.data[i+2] == 0x01) ||
            (i + 5 < map.size &&
             map.data[i] == 0x00 && map.data[i+1] == 0x00 &&
             map.data[i+2] == 0x00 && map.data[i+3] == 0x01))
        {
            /* Skip past start code to the NAL header byte */
            gsize nal_start = (map.data[i+2] == 0x01) ? i + 3 : i + 4;
            if (nal_start < map.size) {
                uint8_t nal_type = (map.data[nal_start] >> 1) & 0x3F;
                if (nal_type == 32) {  /* VPS_NUT */
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

/* ── Pipeline Construction ────────────────────────────────────── */

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

static bool rtmp_plugin_is_streambox(const char *plugin)
{
    return plugin &&
        (g_ascii_strcasecmp(plugin, "streambox") == 0 ||
         g_ascii_strcasecmp(plugin, "srtmp") == 0 ||
         g_ascii_strcasecmp(plugin, "experimental") == 0);
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

static char *resolve_file_location(const sbs_worker_config_t *config)
{
    const char *container = normalized_file_container(config->output.file_container);
    const char *mode = config->output.file_path_mode ? config->output.file_path_mode : "file";
    const char *path = config->output.file_path;

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
    char *prefix = sanitize_file_prefix(config->output.file_prefix);
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

static GstElement *build_output_pipeline(sbs_worker_config_t *config)
{
    const char *codec    = config->output.codec ? config->output.codec : "h265";
    uint32_t    bitrate  = config->output.bitrate;

    GstElement *pipeline = gst_pipeline_new("output-pipeline");
    GstElement *appsrc   = gst_element_factory_make("appsrc", "src");
    GstElement *audio_appsrc = gst_element_factory_make("appsrc", "audio_src");

    if (!pipeline || !appsrc || !audio_appsrc) {
        LOG_E("failed to create pipeline/appsrc elements");
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (audio_appsrc) gst_object_unref(audio_appsrc);
        return NULL;
    }

    /* Configure appsrc — input is NV21 from compositor (RGBA→NV21 conversion
     * done on the compositor side). amlvenc accepts NV21 directly, bypassing
     * ge2d hardware conversion and going straight to the Wave521 VPU. */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "NV21",
        "width",  G_TYPE_INT, (gint)config->width,
        "height", G_TYPE_INT, (gint)config->height,
        "framerate", GST_TYPE_FRACTION,
            (gint)config->framerate_num,
            (gint)(config->framerate_den > 0 ? config->framerate_den : 1),
        NULL);

    g_object_set(appsrc,
        "caps",        caps,
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        "do-timestamp", TRUE,  /* Let GStreamer clock assign timestamps */
        "max-bytes",   (guint64)(config->width * config->height * 3 / 2 * 3),  /* ~3 NV21 frames */
        NULL);
    gst_caps_unref(caps);

    GstCaps *audio_caps = gst_caps_new_simple("audio/x-raw",
        "format", G_TYPE_STRING, "S16LE",
        "rate", G_TYPE_INT, 48000,
        "channels", G_TYPE_INT, 2,
        "layout", G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(audio_appsrc,
        "caps", audio_caps,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        "do-timestamp", FALSE,
        "block", FALSE,
        "max-bytes", 48000 * 2 * 2 * 2,
        NULL);
    gst_caps_unref(audio_caps);

    /* Select encoder */
    const char *parser_name = NULL;
    GstElement *encoder = select_encoder(codec, bitrate,
                                          config->output.encoder,
                                          &parser_name);
    if (!encoder) {
        LOG_E("no encoder available for output worker");
        gst_object_unref(pipeline);
        gst_object_unref(appsrc);
        gst_object_unref(audio_appsrc);
        return NULL;
    }

    /* Create parser — config-interval=-1 inserts SPS/PPS before every
     * keyframe so late-joining SRT receivers can decode immediately. */
    GstElement *parser = gst_element_factory_make(parser_name, "parser");
    if (!parser) {
        LOG_E("failed to create parser: %s", parser_name);
        gst_object_unref(encoder);
        gst_object_unref(pipeline);
        gst_object_unref(appsrc);
        return NULL;
    }
    g_object_set(parser, "config-interval", (gint)-1, NULL);

    /* Create queues between stages (matches production pipeline) */
    GstElement *q1 = gst_element_factory_make("queue", "q1");
    GstElement *q2 = gst_element_factory_make("queue", "q2");
    if (q1) {
        g_object_set(q1,
            "max-size-buffers", (guint)5,
            "max-size-time",    (guint64)0,
            "max-size-bytes",   (guint)0,
            NULL);
    }
    if (q2) {
        g_object_set(q2,
            "max-size-buffers", (guint)30,
            "max-size-time",    (guint64)0,
            "max-size-bytes",   (guint)0,
            NULL);
    }

    /* Create muxer */
    bool is_rtmp = (config->output.sink_type && strcmp(config->output.sink_type, "rtmp") == 0);
    bool is_file = (config->output.sink_type && strcmp(config->output.sink_type, "file") == 0);
    bool is_h264 = (codec && strcmp(codec, "h264") == 0);
    bool is_h265 = (codec && strcmp(codec, "h265") == 0);
    bool streambox_rtmp = is_rtmp && rtmp_plugin_is_streambox(config->output.rtmp_plugin);
    const char *file_container = normalized_file_container(config->output.file_container);
    GstElement *muxer = NULL;
    GstElement *vcapsfilter = NULL;
    if (is_file && strcmp(file_container, "mkv") == 0) {
        muxer = gst_element_factory_make("matroskamux", "mux");
    } else if (is_file && strcmp(file_container, "flv") == 0) {
        muxer = gst_element_factory_make("flvmux", "mux");
    } else if (is_file && strcmp(file_container, "mp4") == 0) {
        muxer = gst_element_factory_make("mp4mux", "mux");
    } else if (is_rtmp && (is_h264 || streambox_rtmp)) {
        muxer = gst_element_factory_make(streambox_rtmp ? "sflvmux" : "flvmux", "mux");
        if (!muxer && streambox_rtmp)
            LOG_E("experimental RTMP requested but sflvmux is unavailable");
    } else if (is_rtmp) {
        LOG_E("RTMP output requires H.264 unless rtmp_plugin=streambox (codec=%s)",
              codec ? codec : "h265");
    } else {
        muxer = gst_element_factory_make("mpegtsmux", "mux");
    }
    GstElement *aq1 = gst_element_factory_make("queue", "aq1");
    GstElement *aconv = gst_element_factory_make("audioconvert", "aconv");
    GstElement *aresample = gst_element_factory_make("audioresample", "aresample");
    GstElement *aenc = gst_element_factory_make("avenc_aac", "aenc");
    if (!aenc) aenc = gst_element_factory_make("voaacenc", "aenc");
    GstElement *aparse = gst_element_factory_make("aacparse", "aparse");
    if (aq1) {
        g_object_set(aq1,
            "max-size-buffers", 0,
            "max-size-bytes", 0,
            "max-size-time", (guint64)(2 * GST_SECOND),
            "leaky", 2,
            NULL);
    }
    if (aenc) {
        g_object_set(aenc, "bitrate", 128000, NULL);
    }
    if (muxer && (!is_rtmp && (!is_file || strcmp(file_container, "ts") == 0))) {
        g_object_set(muxer,
            "alignment", (gint)7,
            "latency",   (guint64)100000000,
            NULL);
    }
    if (muxer && (is_rtmp || (is_file && strcmp(file_container, "flv") == 0)) &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(muxer), "streamable")) {
        g_object_set(muxer, "streamable", TRUE, NULL);
    }
    if (muxer && is_rtmp) {
        GstCaps *vcaps = NULL;
        vcapsfilter = gst_element_factory_make("capsfilter", "vcaps");
        if (is_h265) {
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

    GstElement *sink  = NULL;
    const char *sink_type = config->output.sink_type;
    const char *srt_uri   = config->output.srt_uri;
    const char *rtmp_uri  = config->output.rtmp_uri;
    const char *rtmp_passcode = config->output.rtmp_passcode;
    const char *file_path = config->output.file_path;

    if ((!sink_type || strcmp(sink_type, "srt") == 0) && srt_uri && strlen(srt_uri) > 0) {
        sink = gst_element_factory_make("srtsink", "sink");
        if (sink) {
            g_object_set(sink,
                "uri",                 srt_uri,
                "wait-for-connection", FALSE,
                "sync",               FALSE,
                NULL);
            LOG_I("SRT sink: %s", srt_uri);
        }
    } else if (sink_type && strcmp(sink_type, "rtmp") == 0 && rtmp_uri && strlen(rtmp_uri) > 0) {
        char *location = build_rtmp_location(rtmp_uri, rtmp_passcode);
        sink = gst_element_factory_make(streambox_rtmp ? "srtmpsink" : "rtmp2sink", "sink");
        if (!sink && !streambox_rtmp) {
            sink = gst_element_factory_make("rtmpsink", "sink");
        }
        if (sink) {
            g_object_set(sink,
                "location", location ? location : rtmp_uri,
                "sync",     FALSE,
                NULL);
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "async"))
                g_object_set(sink, "async", FALSE, NULL);
            if (streambox_rtmp && is_h265 &&
                g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "enhanced-codecs"))
                g_object_set(sink, "enhanced-codecs", "hvc1", NULL);
            LOG_I("RTMP sink: %s%s", rtmp_uri,
                  rtmp_passcode && *rtmp_passcode ? " (stream key set)" : "");
            LOG_I("RTMP sink plugin: %s", streambox_rtmp ? "srtmpsink" : GST_OBJECT_NAME(sink));
        } else if (streambox_rtmp) {
            LOG_E("experimental RTMP requested but srtmpsink is unavailable");
        }
        g_free(location);
    } else if (sink_type && strcmp(sink_type, "file") == 0 && file_path && strlen(file_path) > 0) {
        char *location = resolve_file_location(config);
        sink = gst_element_factory_make("filesink", "sink");
        if (sink) {
            g_object_set(sink,
                "location", location ? location : file_path,
                "sync",     FALSE,
                NULL);
            LOG_I("File sink: %s", location ? location : file_path);
        }
        g_free(location);
    } else if (sink_type && strcmp(sink_type, "fakesink") == 0) {
        sink = gst_element_factory_make("fakesink", "sink");
        if (sink) {
            g_object_set(sink, "sync", FALSE, NULL);
            LOG_I("fakesink selected");
        }
    }

    if (!sink) {
        LOG_E("failed to create requested output sink '%s'", sink_type ? sink_type : "srt");
    }

    if (!muxer || !sink || !q1 || !q2 || !aq1 || !aconv || !aresample || !aenc || !aparse ||
        (is_rtmp && !vcapsfilter)) {
        LOG_E("failed to create pipeline elements");
        gst_object_unref(pipeline);
        gst_object_unref(appsrc);
        gst_object_unref(encoder);
        gst_object_unref(parser);
        if (vcapsfilter) gst_object_unref(vcapsfilter);
        if (muxer) gst_object_unref(muxer);
        if (sink) gst_object_unref(sink);
        if (q1) gst_object_unref(q1);
        if (q2) gst_object_unref(q2);
        if (aq1) gst_object_unref(aq1);
        if (aconv) gst_object_unref(aconv);
        if (aresample) gst_object_unref(aresample);
        if (aenc) gst_object_unref(aenc);
        if (aparse) gst_object_unref(aparse);
        return NULL;
    }

    /* Add all elements to the pipeline */
    if (vcapsfilter) {
        gst_bin_add_many(GST_BIN(pipeline), appsrc, q1, encoder, parser, vcapsfilter, q2,
                         audio_appsrc, aq1, aconv, aresample, aenc, aparse,
                         muxer, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), appsrc, q1, encoder, parser, q2,
                         audio_appsrc, aq1, aconv, aresample, aenc, aparse,
                         muxer, sink, NULL);
    }

    /* Link: appsrc → q1 → encoder → parser → q2 → muxer → sink */
    gboolean video_linked = vcapsfilter
        ? gst_element_link_many(appsrc, q1, encoder, parser, vcapsfilter, q2, muxer, sink, NULL)
        : gst_element_link_many(appsrc, q1, encoder, parser, q2, muxer, sink, NULL);
    if (!video_linked) {
        LOG_E("failed to link output pipeline");
        gst_object_unref(pipeline);
        return NULL;
    }
    if (!gst_element_link_many(audio_appsrc, aq1, aconv, aresample, aenc, aparse, muxer, NULL)) {
        LOG_E("failed to link audio branch in output pipeline");
        gst_object_unref(pipeline);
        return NULL;
    }

    /* Install H.265 stream sanitizer probe on h265parse src pad.
     * Drops any buffers emitted before the first VPS NAL, which works
     * around an amlvenc initialization artifact when fed from appsrc. */
    if (strcmp(parser_name, "h265parse") == 0) {
        GstPad *src_pad = gst_element_get_static_pad(parser, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad,
                GST_PAD_PROBE_TYPE_BUFFER,
                h265_stream_sanitize_probe, NULL, NULL);
            gst_object_unref(src_pad);
            LOG_I("h265 stream sanitizer probe installed on parser src pad");
        }
    }

    return pipeline;
}

/* ── CPU Frame Buffer Creation ────────────────────────────────── */

/**
 * Read a frame from a memfd into a plain CPU GstBuffer for the encoder.
 *
 * With the fixed ION allocator in amlvenc (heap type 16 patch), the encoder
 * can now internally allocate its own DMA buffers via ION for the ge2d
 * RGB→NV12 conversion. We no longer need to wrap input buffers as DMA-BUFs
 * from /dev/dma_heap — that workaround was causing the encoder to take a
 * different code path (is_dmabuf[1]) that produced malformed H.265 NAL units.
 *
 * Plain CPU buffers take the is_dmabuf[0] path in amlvenc, which correctly:
 *  1. Copies CPU data into an ION buffer
 *  2. Runs ge2d RGB→NV12 conversion
 *  3. Submits to Wave521 VPU
 *  4. Produces correct VPS→SPS→PPS→IDR NAL ordering
 *
 * GstVideoMeta is added so downstream elements know the exact video layout.
 *
 * The incoming fd (memfd from compositor) is always closed by this function.
 */
static GstBuffer *read_frame_to_buffer(const sbs_video_frame_msg_t *msg,
                                        int memfd)
{
    if (memfd < 0) return NULL;

    /* Calculate total buffer size from plane info.
     * For NV21/NV12 (n_planes=2): the chroma plane has height/2 rows,
     * so we can't simply use stride * height for the last plane. */
    gsize total_size = 0;
    if (msg->n_planes == 2) {
        /* Semi-planar YUV: Y plane = stride[0] * height, chroma = stride[1] * height/2 */
        total_size = (gsize)msg->plane_offset[1] +
                     (gsize)msg->plane_stride[1] * (msg->height / 2);
    } else if (msg->n_planes == 1) {
        total_size = (gsize)msg->plane_stride[0] * msg->height;
    } else if (msg->n_planes > 0) {
        uint32_t last_plane = msg->n_planes - 1;
        if (last_plane >= 4) last_plane = 3;
        total_size = (gsize)msg->plane_offset[last_plane] +
                     (gsize)msg->plane_stride[last_plane] * msg->height;
    }

    /* Fallback size estimate */
    if (total_size == 0) {
        total_size = (gsize)msg->width * msg->height * 3 / 2;  /* NV21: 1.5 bpp */
    }

    /* Try mmap first, fall back to read() */
    void *src_data = mmap(NULL, total_size, PROT_READ, MAP_SHARED, memfd, 0);
    gboolean used_mmap = (src_data != MAP_FAILED);

    if (!used_mmap) {
        src_data = g_malloc(total_size);
        if (!src_data) {
            close(memfd);
            return NULL;
        }
        lseek(memfd, 0, SEEK_SET);
        ssize_t n_read = 0;
        while ((gsize)n_read < total_size) {
            ssize_t n = read(memfd, (uint8_t *)src_data + n_read,
                             total_size - (gsize)n_read);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_W("read memfd failed: %s", strerror(errno));
                g_free(src_data);
                close(memfd);
                return NULL;
            }
            if (n == 0) break;
            n_read += n;
        }
    }

    close(memfd);

    /* Copy into a GLib-managed buffer */
    void *buf_data = g_malloc(total_size);
    if (!buf_data) {
        if (used_mmap) munmap(src_data, total_size);
        else g_free(src_data);
        return NULL;
    }
    memcpy(buf_data, src_data, total_size);

    if (used_mmap) munmap(src_data, total_size);
    else g_free(src_data);

    /* Wrap in GstBuffer */
    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer,
        gst_memory_new_wrapped(0, buf_data, total_size, 0, total_size,
                               buf_data, g_free));

    /* Do NOT add GstVideoMeta — videotestsrc doesn't add it for RGB,
     * and amlvenc identifies format purely from negotiated caps.
     * Adding extra meta may confuse the encoder's internal buffer handling. */

    /* Timestamps are handled by appsrc (do-timestamp=TRUE), which uses
     * the pipeline running clock. This matches how videotestsrc works and
     * ensures proper segment/timing coordination with downstream elements.
     * Manual PTS/DTS assignment causes amlvenc to crash at the second GOP
     * boundary (after ~30 frames) because amlvenc's internal timing logic
     * relies on synchronization with the GStreamer pipeline clock. */

    return buffer;
}

/* ── IPC Frame Reception (GLib I/O Watch) ─────────────────────── */

static gboolean on_frame_received(gint fd, GIOCondition cond, gpointer user_data)
{
    output_state_t *state = user_data;
    (void)fd;

    if (state->ctx->shutting_down) {
        return G_SOURCE_REMOVE;
    }

    if (cond & (G_IO_HUP | G_IO_ERR)) {
        LOG_W("supervisor connection lost (HUP/ERR)");
        state->io_watch_id = 0;
        sbs_worker_request_shutdown(state->ctx);
        return G_SOURCE_REMOVE;
    }

    sbs_ipc_msg_header_t hdr;
    ssize_t peek = recv(state->ctx->sock_fd, &hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT);
    if (peek < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return G_SOURCE_CONTINUE;
        LOG_W("recv peek failed: %s", strerror(errno));
        return G_SOURCE_CONTINUE;
    }
    if (peek == 0) {
        state->io_watch_id = 0;
        sbs_worker_request_shutdown(state->ctx);
        return G_SOURCE_REMOVE;
    }

    if (hdr.msg_type == SBS_IPC_MSG_AUDIO_BUFFER) {
        uint8_t packet[sizeof(sbs_audio_buffer_msg_t) + 8192];
        size_t read = 0;
        int rc = sbs_ipc_recv_msg(state->ctx->sock_fd, packet, sizeof(packet), &read);
        if (rc != SBS_OK) {
            return G_SOURCE_CONTINUE;
        }
        if (read >= sizeof(sbs_audio_buffer_msg_t)) {
            sbs_audio_buffer_msg_t *amsg = (sbs_audio_buffer_msg_t *)packet;
            GstBuffer *buffer = gst_buffer_new_allocate(NULL, amsg->data_size, NULL);
            gst_buffer_fill(buffer, 0, packet + sizeof(*amsg), amsg->data_size);
            GST_BUFFER_PTS(buffer) = amsg->pts_ns;
            GST_BUFFER_DURATION(buffer) = amsg->duration_ns;
            gst_app_src_push_buffer(GST_APP_SRC(state->audio_appsrc), buffer);
            state->audio_buffers_consumed++;
        }
        return G_SOURCE_CONTINUE;
    }

    /* Receive frame message + DMA-BUF fd */
    sbs_video_frame_msg_t msg;
    int dmabuf_fd = -1;

    int rc = sbs_ipc_recv_frame(state->ctx->sock_fd, &msg, &dmabuf_fd);

    if (rc == SBS_ERR_WOULD_BLOCK) {
        return G_SOURCE_CONTINUE;
    }

    if (rc == SBS_ERR_EOF) {
        LOG_I("supervisor connection closed");
        state->io_watch_id = 0;
        sbs_worker_request_shutdown(state->ctx);
        return G_SOURCE_REMOVE;
    }

    if (rc != SBS_OK) {
        LOG_W("recv_frame error: %d", rc);
        return G_SOURCE_CONTINUE;
    }

    /* Handle shutdown messages */
    if (msg.header.msg_type == SBS_IPC_MSG_SHUTDOWN) {
        LOG_I("received SHUTDOWN from supervisor");
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        sbs_worker_request_shutdown(state->ctx);
        return G_SOURCE_CONTINUE;
    }

    /* Only process composed frames */
    if (msg.header.msg_type != SBS_IPC_MSG_COMPOSED_FRAME &&
        msg.header.msg_type != SBS_IPC_MSG_VIDEO_FRAME) {
        LOG_D("ignoring msg type 0x%04x", msg.header.msg_type);
        if (dmabuf_fd >= 0) close(dmabuf_fd);
        return G_SOURCE_CONTINUE;
    }

    state->frames_consumed++;

    /* Read frame into a plain CPU GstBuffer for the encoder */
    GstBuffer *buffer = read_frame_to_buffer(&msg, dmabuf_fd);
    if (!buffer) {
        state->frames_dropped++;
        return G_SOURCE_CONTINUE;
    }

    /* Push buffer to appsrc */
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(state->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        LOG_W("appsrc push failed: %s", gst_flow_get_name(ret));
        state->frames_dropped++;
    }

    return G_SOURCE_CONTINUE;
}

/* ── Bus Message Handler ──────────────────────────────────────── */

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    output_state_t *state = user_data;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(msg, &err, &debug);

        LOG_E("pipeline error: %s", err->message);
        if (debug) {
            LOG_D("debug: %s", debug);
            g_free(debug);
        }

        /* Send error status to supervisor */
        sbs_status_msg_t status;
        sbs_status_msg_init(&status);
        status.state = (uint32_t)SBS_WORKER_STATE_ERROR;
        strncpy(status.error_message, err->message, sizeof(status.error_message) - 1);
        sbs_ipc_send_msg(state->ctx->sock_fd, &status, sizeof(status));

        g_error_free(err);
        sbs_worker_request_shutdown(state->ctx);
        break;
    }

    case GST_MESSAGE_EOS:
        LOG_I("end of stream");
        sbs_worker_send_status(state->ctx, SBS_WORKER_STATE_EOS);
        sbs_worker_request_shutdown(state->ctx);
        break;

    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(state->pipeline)) {
            GstState old, new_state, pending;
            gst_message_parse_state_changed(msg, &old, &new_state, &pending);
            LOG_D("pipeline state: %s → %s",
                  gst_element_state_get_name(old),
                  gst_element_state_get_name(new_state));
        }
        break;

    default:
        break;
    }

    return TRUE;
}

/* ── Heartbeat Timer ──────────────────────────────────────────── */

static gboolean on_heartbeat(gpointer user_data)
{
    output_state_t *state = user_data;

    if (state->ctx->shutting_down) {
        return G_SOURCE_REMOVE;
    }

    sbs_status_msg_t status;
    sbs_status_msg_init(&status);
    status.state = (uint32_t)SBS_WORKER_STATE_RUNNING;
    status.frame_width    = state->ctx->config.width;
    status.frame_height   = state->ctx->config.height;
    status.frame_rate_num = state->ctx->config.framerate_num;
    status.frame_rate_den = state->ctx->config.framerate_den;
    status.frames_consumed = state->frames_consumed;
    status.frames_encoded  = state->frames_encoded;
    status.frames_dropped  = state->frames_dropped;
    status.bytes_written   = state->bytes_written;
    status.encoder_bitrate = state->ctx->config.output.bitrate;

    int rc = sbs_ipc_send_msg(state->ctx->sock_fd, &status, sizeof(status));
    if (rc != SBS_OK && rc != SBS_ERR_WOULD_BLOCK) {
        LOG_W("heartbeat send failed: %d", rc);
    }

    LOG_D("heartbeat: %lu consumed, %lu dropped",
          (unsigned long)state->frames_consumed,
          (unsigned long)state->frames_dropped);

    return G_SOURCE_CONTINUE;
}

/* ── Output Worker Entry Point ────────────────────────────────── */

int output_worker_run(sbs_worker_ctx_t *ctx)
{
    output_state_t state = {
        .ctx             = ctx,
        .pipeline        = NULL,
        .appsrc          = NULL,
        .audio_appsrc    = NULL,
        .frames_consumed = 0,
        .audio_buffers_consumed = 0,
        .frames_encoded  = 0,
        .frames_dropped  = 0,
        .bytes_written   = 0,
        .heartbeat_timer = 0,
        .io_watch_id     = 0,
    };

    LOG_I("building output pipeline: %ux%u@%u/%u codec=%s bitrate=%u srt=%s",
          ctx->config.width, ctx->config.height,
          ctx->config.framerate_num, ctx->config.framerate_den,
          ctx->config.output.codec ? ctx->config.output.codec : "h265",
          ctx->config.output.bitrate,
          ctx->config.output.srt_uri ? ctx->config.output.srt_uri : "(none)");

    /* Build the GStreamer pipeline.
     * With the ION allocator fix (heap type 16 patch in gstamlionallocator.c),
     * amlvenc can internally allocate DMA buffers for ge2d RGB→NV12 conversion.
     * We send plain CPU buffers — no DMA heap wrapping needed. */
    state.pipeline = build_output_pipeline(&ctx->config);
    if (!state.pipeline) {
        LOG_E("pipeline construction failed");
        return SBS_ERR_IO;
    }

    /* Get appsrc reference */
    state.appsrc = gst_bin_get_by_name(GST_BIN(state.pipeline), "src");
    state.audio_appsrc = gst_bin_get_by_name(GST_BIN(state.pipeline), "audio_src");
    if (!state.appsrc || !state.audio_appsrc) {
        LOG_E("video/audio appsrc not found in pipeline");
        gst_object_unref(state.pipeline);
        return SBS_ERR_IO;
    }

    /* Install bus watch */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(state.pipeline));
    gst_bus_add_watch(bus, on_bus_message, &state);
    gst_object_unref(bus);

    /* Start pipeline */
    GstStateChangeReturn ret = gst_element_set_state(state.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_E("failed to set pipeline to PLAYING");
        gst_object_unref(state.appsrc);
        gst_object_unref(state.pipeline);
        return SBS_ERR_IO;
    }

    /* Install GLib I/O watch on supervisor socket for incoming frames */
    state.io_watch_id = g_unix_fd_add(ctx->sock_fd,
                                       G_IO_IN | G_IO_HUP | G_IO_ERR,
                                       on_frame_received, &state);

    /* Send RUNNING status */
    sbs_worker_send_status(ctx, SBS_WORKER_STATE_RUNNING);

    /* Start heartbeat timer */
    uint32_t hb_ms = ctx->config.heartbeat_interval_ms;
    if (hb_ms == 0) hb_ms = 5000;
    state.heartbeat_timer = g_timeout_add(hb_ms, on_heartbeat, &state);

    LOG_I("output pipeline PLAYING — starting main loop");

    /* Run main loop (blocks until shutdown) */
    sbs_worker_run(ctx);

    /* Cleanup */
    LOG_I("shutting down output pipeline...");

    if (state.heartbeat_timer) {
        g_source_remove(state.heartbeat_timer);
        state.heartbeat_timer = 0;
    }

    if (state.io_watch_id) {
        g_source_remove(state.io_watch_id);
        state.io_watch_id = 0;
    }

    /* Signal EOS to pipeline and wait for it to propagate */
    gst_app_src_end_of_stream(GST_APP_SRC(state.appsrc));
    gst_app_src_end_of_stream(GST_APP_SRC(state.audio_appsrc));

    /* Drain pipeline — wait for EOS to reach sink so filesink flushes */
    GstBus *drain_bus = gst_element_get_bus(state.pipeline);
    gst_element_set_state(state.pipeline, GST_STATE_PLAYING);
    GstMessage *eos_msg = gst_bus_timed_pop_filtered(drain_bus,
        2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (eos_msg) {
        gst_message_unref(eos_msg);
    }
    gst_object_unref(drain_bus);

    gst_element_set_state(state.pipeline, GST_STATE_NULL);

    /* Remove bus watch */
    bus = gst_pipeline_get_bus(GST_PIPELINE(state.pipeline));
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);

    gst_object_unref(state.appsrc);
    gst_object_unref(state.audio_appsrc);
    gst_object_unref(state.pipeline);

    LOG_I("output worker done: %lu consumed, %lu dropped",
          (unsigned long)state.frames_consumed,
          (unsigned long)state.frames_dropped);

    return SBS_OK;
}
