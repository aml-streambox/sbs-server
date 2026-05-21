/*
 * SBS - StreamBox Broadcast System
 * Source worker — GStreamer capture pipeline → IPC frame delivery
 *
 * Builds a GStreamer pipeline based on source_type (videotestsrc,
 * v4l2src, uridecodebin, image), extracts video frames from appsink, and sends them with DMA-BUF fds
 * (or memfd for CPU buffers) to the supervisor over the IPC socket.
 *
 * References: document/05-source-manager.md sections 2, 5
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "source"

#include "source_worker.h"
#include "sbs/dmabuf_alloc.h"
#include "sbs/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>  /* memfd_create */
#include <linux/videodev2.h>

#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <drm/drm_fourcc.h>
#include <vfmcap.h>

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif
#ifndef SBS_DRM_FORMAT_AMLY
#define SBS_DRM_FORMAT_AMLY 0x594c4d41u
#endif

#define SBS_VFMCAP_MAX_IN_FLIGHT 16
#define SBS_VFMCAP_BUFFER_COUNT 16
#define SBS_VFMCAP_ACQUIRE_TIMEOUT_MS 2
#define SBS_VFMCAP_NOSIG_RECOVER_THRESHOLD 120
#define SBS_VFMCAP_RECOVER_INTERVAL_MS 2000
#define SBS_VFMCAP_STABILITY_CHECK_COUNT 3
#define SBS_VFMCAP_LOW_FPS_RECOVER_WINDOWS 4
#define SBS_VFMCAP_DEFAULT_DEVICE "/dev/video_cap"

typedef struct source_vfmcap_lease {
    bool active;
    uint64_t sequence;
    vfmcap_frame_t frame;
} source_vfmcap_lease_t;

/* ── Source Worker State ──────────────────────────────────────── */

typedef struct source_state {
    sbs_worker_ctx_t  *ctx;
    GstElement        *pipeline;
    GstElement        *appsink;
    uint64_t           frame_counter;
    uint64_t           frames_dropped;
    guint              heartbeat_timer;
    guint              text_frame_timer;
    sbs_dmabuf_alloc_t dmabuf_alloc;
    bool               dmabuf_available;

#define SBS_CPU_EXPORT_RING_SIZE 8
    struct {
        sbs_dmabuf_buffer_t backing;
        void               *mapped;
        size_t              size;
    } cpu_export_ring[SBS_CPU_EXPORT_RING_SIZE];
    uint32_t           cpu_export_ring_idx;

    /* vfmcap direct path (bypasses GStreamer) */
    vfmcap_ctx_t      *vfmcap_ctx;
    vfmcap_config_t    vfmcap_config;
    vfmcap_output_fmt_t vfmcap_output_format;
    guint              vfmcap_frame_timer;
    guint              vfmcap_event_timer;
    guint              vfmcap_stats_timer;
    guint              vfmcap_recover_timer;
    guint              socket_watch_id;
    bool               vfmcap_recovering;
    uint32_t           vfmcap_consecutive_nosig;
    uint32_t           vfmcap_stable_count;
    uint32_t           vfmcap_stable_width;
    uint32_t           vfmcap_stable_height;
    source_vfmcap_lease_t vfmcap_leases[SBS_VFMCAP_MAX_IN_FLIGHT];

    int64_t            vfmcap_stats_last_us;
    int64_t            vfmcap_last_success_us;
    uint64_t           vfmcap_stats_last_frame_counter;
    uint64_t           vfmcap_stats_last_drop_counter;
    uint64_t           vfmcap_acquire_attempts;
    uint64_t           vfmcap_acquire_ok;
    uint64_t           vfmcap_acquire_timeouts;
    uint64_t           vfmcap_acquire_nosig;
    uint64_t           vfmcap_acquire_errors;
    uint64_t           vfmcap_acquire_reconfigured;
    uint64_t           vfmcap_acquire_total_us;
    uint64_t           vfmcap_acquire_max_us;
    uint64_t           vfmcap_success_intervals;
    uint64_t           vfmcap_success_interval_total_us;
    uint64_t           vfmcap_success_interval_max_us;
    uint64_t           vfmcap_no_lease_skips;
    uint64_t           vfmcap_no_fd_drops;
    uint64_t           vfmcap_split_plane_drops;
    uint64_t           vfmcap_lease_full_drops;
    uint64_t           vfmcap_send_would_block;
    uint64_t           vfmcap_send_errors;
    uint64_t           vfmcap_release_acks;
    uint64_t           vfmcap_unknown_release_acks;
    uint32_t           vfmcap_max_in_flight;
    uint32_t           vfmcap_low_fps_windows;
} source_state_t;

static void source_worker_release_cpu_export_ring(source_state_t *state)
{
    for (uint32_t i = 0; i < SBS_CPU_EXPORT_RING_SIZE; i++) {
        if (state->cpu_export_ring[i].mapped) {
            munmap(state->cpu_export_ring[i].mapped, state->cpu_export_ring[i].size);
            state->cpu_export_ring[i].mapped = NULL;
        }
        if (state->cpu_export_ring[i].backing.fd >= 0) {
            close(state->cpu_export_ring[i].backing.fd);
            state->cpu_export_ring[i].backing.fd = -1;
        }
        state->cpu_export_ring[i].size = 0;
    }
}

static int source_worker_acquire_cpu_export_slot(source_state_t *state,
                                                 size_t size,
                                                 void **mapped_out,
                                                 sbs_dmabuf_buffer_t **buf_out)
{
    uint32_t idx = state->cpu_export_ring_idx++ % SBS_CPU_EXPORT_RING_SIZE;
    sbs_dmabuf_buffer_t new_buf = { .fd = -1 };
    void *new_map = NULL;
    if (!state->dmabuf_available)
        return -1;

    if (state->cpu_export_ring[idx].size != size ||
        state->cpu_export_ring[idx].backing.fd < 0 ||
        state->cpu_export_ring[idx].mapped == NULL) {
        if (state->cpu_export_ring[idx].mapped) {
            munmap(state->cpu_export_ring[idx].mapped, state->cpu_export_ring[idx].size);
            state->cpu_export_ring[idx].mapped = NULL;
        }
        if (state->cpu_export_ring[idx].backing.fd >= 0) {
            close(state->cpu_export_ring[idx].backing.fd);
            state->cpu_export_ring[idx].backing.fd = -1;
        }

        if (sbs_dmabuf_alloc_buffer_from_heap(&state->dmabuf_alloc,
                                              SBS_DMABUF_HEAP_SYSTEM,
                                              size, 0, &new_buf) != SBS_OK ||
            new_buf.fd < 0) {
            if (new_buf.fd >= 0)
                close(new_buf.fd);
            new_buf.fd = -1;
            (void)sbs_dmabuf_alloc_buffer(&state->dmabuf_alloc, size, 0, &new_buf);
        }
        if (new_buf.fd < 0 || new_buf.heap == SBS_DMABUF_HEAP_MEMFD) {
            if (new_buf.fd >= 0)
                close(new_buf.fd);
            return -1;
        }

        new_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, new_buf.fd, 0);
        if (new_map == MAP_FAILED) {
            close(new_buf.fd);
            return -1;
        }

        state->cpu_export_ring[idx].backing = new_buf;
        state->cpu_export_ring[idx].mapped = new_map;
        state->cpu_export_ring[idx].size = size;
    }

    *mapped_out = state->cpu_export_ring[idx].mapped;
    *buf_out = &state->cpu_export_ring[idx].backing;
    return dup(state->cpu_export_ring[idx].backing.fd);
}

/* ── Helpers ───────────────────────────────────────────────────── */

/**
 * Lookup a GStreamer enum value by nick string.
 * Used to convert pattern names like "smpte" to their GEnum integer value.
 */
static gint gst_video_test_src_pattern_from_string(const char *nick)
{
    /* Try to look up the enum type from the videotestsrc element.
     * If not available, default to 0 (smpte). */
    GstElementFactory *factory = gst_element_factory_find("videotestsrc");
    if (!factory) return 0;

    GstElement *tmp = gst_element_factory_create(factory, NULL);
    gst_object_unref(factory);
    if (!tmp) return 0;

    GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(tmp), "pattern");
    gst_object_unref(tmp);

    if (!pspec || !G_IS_PARAM_SPEC_ENUM(pspec)) return 0;

    GEnumClass *eclass = G_PARAM_SPEC_ENUM(pspec)->enum_class;
    GEnumValue *eval = g_enum_get_value_by_nick(eclass, nick);
    return eval ? eval->value : 0;
}

static gchar *source_uri_from_config(const char *uri_or_path)
{
    GError *err = NULL;
    gchar *uri;

    if (!uri_or_path || uri_or_path[0] == '\0') {
        return NULL;
    }
    if (gst_uri_is_valid(uri_or_path)) {
        return g_strdup(uri_or_path);
    }

    uri = gst_filename_to_uri(uri_or_path, &err);
    if (!uri) {
        LOG_E("invalid source path '%s': %s",
              uri_or_path, err ? err->message : "unknown error");
        if (err) g_error_free(err);
    }
    return uri;
}

static bool parse_text_color(const char *value, double *r, double *g, double *b, double *a)
{
    const char *s = value;
    uint32_t hex = 0;
    size_t len;

    if (r) *r = 1.0;
    if (g) *g = 1.0;
    if (b) *b = 1.0;
    if (a) *a = 1.0;
    if (!s || !*s)
        return false;
    if (*s == '#')
        s++;
    len = strlen(s);
    if (len != 6 && len != 8)
        return false;
    for (size_t i = 0; i < len; i++) {
        int v = g_ascii_xdigit_value(s[i]);
        if (v < 0)
            return false;
        hex = (hex << 4) | (uint32_t)v;
    }
    if (len == 6) {
        if (r) *r = ((hex >> 16) & 0xffu) / 255.0;
        if (g) *g = ((hex >> 8) & 0xffu) / 255.0;
        if (b) *b = (hex & 0xffu) / 255.0;
    } else {
        if (r) *r = ((hex >> 24) & 0xffu) / 255.0;
        if (g) *g = ((hex >> 16) & 0xffu) / 255.0;
        if (b) *b = ((hex >> 8) & 0xffu) / 255.0;
        if (a) *a = (hex & 0xffu) / 255.0;
    }
    return true;
}

static char *font_family_from_file(const char *path)
{
    FcPattern *pattern;
    FcChar8 *family = NULL;
    char *result = NULL;

    if (!path || !*path)
        return NULL;
    pattern = FcFreeTypeQuery((const FcChar8 *)path, 0, NULL, NULL);
    if (!pattern)
        return NULL;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch && family)
        result = g_strdup((const char *)family);
    FcPatternDestroy(pattern);
    return result;
}

static const char *text_align_from_config(const char *align, PangoAlignment *out)
{
    if (!out)
        return "left";
    if (align && strcmp(align, "center") == 0) {
        *out = PANGO_ALIGN_CENTER;
        return "center";
    }
    if (align && strcmp(align, "right") == 0) {
        *out = PANGO_ALIGN_RIGHT;
        return "right";
    }
    *out = PANGO_ALIGN_LEFT;
    return "left";
}

static int render_text_rgba(sbs_worker_config_t *config, uint8_t *dst,
                            uint32_t width, uint32_t height)
{
    size_t size = (size_t)width * (size_t)height * 4u;
    uint8_t *cairo_data;
    cairo_surface_t *surface;
    cairo_t *cr;
    PangoLayout *layout;
    PangoFontDescription *desc;
    PangoAlignment alignment;
    double r, g, b, a;
    char *font_from_file = NULL;
    const char *font_family;
    uint32_t font_size;

    if (!config || !dst || width == 0 || height == 0)
        return -1;

    cairo_data = g_malloc0(size);
    if (!cairo_data)
        return -1;

    if (config->source.font_path && config->source.font_path[0]) {
        if (FcConfigAppFontAddFile(NULL, (const FcChar8 *)config->source.font_path))
            FcConfigBuildFonts(NULL);
        font_from_file = font_family_from_file(config->source.font_path);
    }

    font_family = font_from_file && font_from_file[0]
        ? font_from_file
        : (config->source.font_family && config->source.font_family[0]
            ? config->source.font_family : "Liberation Sans");
    font_size = config->source.font_size > 0 ? config->source.font_size : 72u;
    parse_text_color(config->source.text_color, &r, &g, &b, &a);

    surface = cairo_image_surface_create_for_data(cairo_data, CAIRO_FORMAT_ARGB32,
                                                  (int)width, (int)height,
                                                  (int)(width * 4u));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        g_free(font_from_file);
        g_free(cairo_data);
        return -1;
    }

    cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, r, g, b, a);

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout,
                          config->source.text && config->source.text[0]
                              ? config->source.text : "StreamBox",
                          -1);
    pango_layout_set_width(layout, (int)width * PANGO_SCALE);
    pango_layout_set_height(layout, (int)height * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    text_align_from_config(config->source.text_align, &alignment);
    pango_layout_set_alignment(layout, alignment);

    desc = pango_font_description_new();
    pango_font_description_set_family(desc, font_family);
    pango_font_description_set_absolute_size(desc, (double)font_size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_cairo_update_layout(cr, layout);
    cairo_move_to(cr, 0.0, 0.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *src = cairo_data + (size_t)y * width * 4u;
        uint8_t *out = dst + (size_t)y * width * 4u;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t cb = src[x * 4u + 0u];
            uint8_t cg = src[x * 4u + 1u];
            uint8_t crv = src[x * 4u + 2u];
            uint8_t ca = src[x * 4u + 3u];
            if (ca > 0) {
                out[x * 4u + 0u] = (uint8_t)MIN(255u, ((uint32_t)crv * 255u + ca / 2u) / ca);
                out[x * 4u + 1u] = (uint8_t)MIN(255u, ((uint32_t)cg * 255u + ca / 2u) / ca);
                out[x * 4u + 2u] = (uint8_t)MIN(255u, ((uint32_t)cb * 255u + ca / 2u) / ca);
                out[x * 4u + 3u] = ca;
            } else {
                out[x * 4u + 0u] = 0;
                out[x * 4u + 1u] = 0;
                out[x * 4u + 2u] = 0;
                out[x * 4u + 3u] = 0;
            }
        }
    }

    cairo_surface_destroy(surface);
    g_free(font_from_file);
    g_free(cairo_data);
    return 0;
}

static void on_decodebin_pad_added(GstElement *src, GstPad *pad, gpointer user_data)
{
    GstElement *target = GST_ELEMENT(user_data);
    GstCaps *caps;
    const GstStructure *structure;
    const char *name;
    GstPad *sinkpad;
    GstPadLinkReturn ret;
    (void)src;

    caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, NULL);
    }
    if (!caps || gst_caps_get_size(caps) == 0) {
        if (caps) gst_caps_unref(caps);
        return;
    }

    structure = gst_caps_get_structure(caps, 0);
    name = gst_structure_get_name(structure);
    if (!name || !g_str_has_prefix(name, "video/")) {
        gst_caps_unref(caps);
        return;
    }
    gst_caps_unref(caps);

    sinkpad = gst_element_get_static_pad(target, "sink");
    if (!sinkpad) {
        return;
    }
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        LOG_W("failed to link decoded video pad: %s", gst_pad_link_get_name(ret));
    }
    gst_object_unref(sinkpad);
}

static GstCaps *build_nv12_caps(sbs_worker_config_t *config)
{
    return gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width",  G_TYPE_INT, (gint)config->width,
        "height", G_TYPE_INT, (gint)config->height,
        "framerate", GST_TYPE_FRACTION,
            (gint)config->framerate_num,
            (gint)(config->framerate_den > 0 ? config->framerate_den : 1),
        NULL);
}

static GstCaps *build_rgba_caps(sbs_worker_config_t *config)
{
    return gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "RGBA",
        "width",  G_TYPE_INT, (gint)config->width,
        "height", G_TYPE_INT, (gint)config->height,
        "framerate", GST_TYPE_FRACTION,
            (gint)config->framerate_num,
            (gint)(config->framerate_den > 0 ? config->framerate_den : 1),
        NULL);
}

static void configure_source_appsink(GstElement *sink, gboolean sync)
{
    g_object_set(sink,
        "emit-signals", TRUE,
        "max-buffers",  2,
        "drop",         TRUE,
        "sync",         sync,
        NULL);
}

/* ── Pipeline Builders ────────────────────────────────────────── */

/**
 * Build videotestsrc pipeline:
 *   videotestsrc pattern=PATTERN ! capsfilter ! appsink
 *
 * CPU-only buffers (no DMA-BUF).
 */
static GstElement *build_videotestsrc_pipeline(sbs_worker_config_t *config)
{
    GstElement *pipeline = gst_pipeline_new("source-pipeline");
    GstElement *src      = gst_element_factory_make("videotestsrc", "src");
    GstElement *capsf    = gst_element_factory_make("capsfilter", "caps");
    GstElement *sink     = gst_element_factory_make("appsink", "sink");

    if (!pipeline || !src || !capsf || !sink) {
        LOG_E("failed to create videotestsrc pipeline elements");
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (capsf) gst_object_unref(capsf);
        if (sink) gst_object_unref(sink);
        return NULL;
    }

    /* Set pattern */
    const char *pattern = config->source.pattern;
    if (pattern && strlen(pattern) > 0) {
        g_object_set(src, "pattern", gst_video_test_src_pattern_from_string(pattern), NULL);
    }

    /* Set caps: NV12 at configured resolution and framerate */
    GstCaps *caps = build_nv12_caps(config);
    g_object_set(capsf, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* Configure appsink: emit-signals, max-buffers=2, drop=true, sync=true */
    configure_source_appsink(sink, TRUE);

    gst_bin_add_many(GST_BIN(pipeline), src, capsf, sink, NULL);
    if (!gst_element_link_many(src, capsf, sink, NULL)) {
        LOG_E("failed to link videotestsrc pipeline");
        gst_object_unref(pipeline);
        return NULL;
    }

    return pipeline;
}

static GstElement *build_decode_pipeline(sbs_worker_config_t *config, bool freeze_image)
{
    GstElement *pipeline = gst_pipeline_new("source-pipeline");
    GstElement *src      = gst_element_factory_make("uridecodebin", "src");
    GstElement *queue    = gst_element_factory_make("queue", "decode_queue");
    GstElement *freeze   = freeze_image ? gst_element_factory_make("imagefreeze", "freeze") : NULL;
    GstElement *conv     = gst_element_factory_make("videoconvert", "convert");
    GstElement *scale    = gst_element_factory_make("videoscale", "scale");
    GstElement *capsf    = gst_element_factory_make("capsfilter", "caps");
    GstElement *sink     = gst_element_factory_make("appsink", "sink");
    gchar *uri = source_uri_from_config(config->source.uri);

    if (!pipeline || !src || !queue || (freeze_image && !freeze) || !conv || !scale || !capsf || !sink) {
        LOG_E("failed to create decode source pipeline elements");
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (queue) gst_object_unref(queue);
        if (freeze) gst_object_unref(freeze);
        if (conv) gst_object_unref(conv);
        if (scale) gst_object_unref(scale);
        if (capsf) gst_object_unref(capsf);
        if (sink) gst_object_unref(sink);
        g_free(uri);
        return NULL;
    }
    if (!uri) {
        LOG_E("%s source requires a non-empty uri/path", freeze_image ? "image" : "uridecodebin");
        gst_object_unref(pipeline);
        gst_object_unref(src);
        gst_object_unref(queue);
        if (freeze) gst_object_unref(freeze);
        gst_object_unref(conv);
        gst_object_unref(scale);
        gst_object_unref(capsf);
        gst_object_unref(sink);
        g_free(uri);
        return NULL;
    }

    g_object_set(src, "uri", uri, NULL);
    g_free(uri);

    if (freeze && g_object_class_find_property(G_OBJECT_GET_CLASS(freeze), "is-live")) {
        g_object_set(freeze, "is-live", TRUE, NULL);
    }

    GstCaps *caps = freeze_image ? build_rgba_caps(config) : build_nv12_caps(config);
    g_object_set(capsf, "caps", caps, NULL);
    gst_caps_unref(caps);
    configure_source_appsink(sink, TRUE);

    if (freeze_image) {
        gst_bin_add_many(GST_BIN(pipeline), src, queue, freeze, conv, scale, capsf, sink, NULL);
        if (!gst_element_link_many(queue, freeze, conv, scale, capsf, sink, NULL)) {
            LOG_E("failed to link image source pipeline");
            gst_object_unref(pipeline);
            return NULL;
        }
    } else {
        gst_bin_add_many(GST_BIN(pipeline), src, queue, conv, scale, capsf, sink, NULL);
        if (!gst_element_link_many(queue, conv, scale, capsf, sink, NULL)) {
            LOG_E("failed to link URI source pipeline");
            gst_object_unref(pipeline);
            return NULL;
        }
    }

    g_signal_connect(src, "pad-added", G_CALLBACK(on_decodebin_pad_added), queue);
    return pipeline;
}

/**
 * Build v4l2src pipeline:
 *   v4l2src device=PATH io-mode=dmabuf ! capsfilter ! appsink
 */
static const char *v4l2_raw_gst_format(const char *fourcc)
{
    if (!fourcc || !fourcc[0]) return NULL;
    if (g_strcmp0(fourcc, "NV12") == 0) return "NV12";
    if (g_strcmp0(fourcc, "NV21") == 0) return "NV21";
    if (g_strcmp0(fourcc, "P010") == 0) return "P010_10LE";
    if (g_strcmp0(fourcc, "RGBA") == 0) return "RGBA";
    if (g_strcmp0(fourcc, "YUYV") == 0) return "YUY2";
    if (g_strcmp0(fourcc, "UYVY") == 0) return "UYVY";
    return NULL;
}

static bool v4l2_raw_format_direct_importable(const char *fourcc)
{
    return g_strcmp0(fourcc, "NV12") == 0 ||
           g_strcmp0(fourcc, "NV21") == 0 ||
           g_strcmp0(fourcc, "P010") == 0 ||
           g_strcmp0(fourcc, "RGBA") == 0;
}

static bool v4l2_format_is_mjpeg(const char *fourcc)
{
    return g_strcmp0(fourcc, "MJPG") == 0 || g_strcmp0(fourcc, "JPEG") == 0;
}

static bool v4l2_format_is_h264(const char *fourcc)
{
    return g_strcmp0(fourcc, "H264") == 0 || g_strcmp0(fourcc, "AVC1") == 0;
}

static bool parse_framerate_string(const char *value, gint fallback_num,
                                   gint fallback_den, gint *out_num, gint *out_den)
{
    unsigned int num = 0;
    unsigned int den = 1;

    if (value && value[0] && sscanf(value, "%u/%u", &num, &den) >= 1 && num > 0) {
        *out_num = (gint)num;
        *out_den = den > 0 ? (gint)den : 1;
        return true;
    }

    *out_num = fallback_num > 0 ? fallback_num : 30;
    *out_den = fallback_den > 0 ? fallback_den : 1;
    return false;
}

static GstCaps *v4l2_raw_caps(const char *format, uint32_t width, uint32_t height,
                              gint fps_num, gint fps_den)
{
    GstCaps *caps = gst_caps_new_empty_simple("video/x-raw");
    const char *gst_format = v4l2_raw_gst_format(format);

    if (gst_format)
        gst_caps_set_simple(caps, "format", G_TYPE_STRING, gst_format, NULL);
    if (width > 0)
        gst_caps_set_simple(caps, "width", G_TYPE_INT, (gint)width, NULL);
    if (height > 0)
        gst_caps_set_simple(caps, "height", G_TYPE_INT, (gint)height, NULL);
    if (fps_num > 0 && fps_den > 0)
        gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, fps_num, fps_den, NULL);
    return caps;
}

static GstCaps *v4l2_compressed_caps(const char *format, uint32_t width, uint32_t height,
                                     gint fps_num, gint fps_den)
{
    GstCaps *caps;
    if (v4l2_format_is_h264(format)) {
        caps = gst_caps_new_empty_simple("video/x-h264");
        gst_caps_set_simple(caps,
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            NULL);
    } else {
        caps = gst_caps_new_empty_simple("image/jpeg");
    }
    if (width > 0)
        gst_caps_set_simple(caps, "width", G_TYPE_INT, (gint)width, NULL);
    if (height > 0)
        gst_caps_set_simple(caps, "height", G_TYPE_INT, (gint)height, NULL);
    if (fps_num > 0 && fps_den > 0)
        gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, fps_num, fps_den, NULL);
    return caps;
}

static const char *v4l2_decoder_factory_name(const char *format, const char *decode_mode,
                                             bool *out_hardware)
{
    const char *hw = v4l2_format_is_h264(format) ? "amlv4l2h264dec" : "amlv4l2jpegdec";
    const char *sw = v4l2_format_is_h264(format) ? "avdec_h264" : "jpegdec";
    bool request_hardware = g_strcmp0(decode_mode, "hardware") == 0;
    bool request_software = g_strcmp0(decode_mode, "software") == 0;

    if (out_hardware)
        *out_hardware = false;

    if (!request_software) {
        GstElementFactory *factory = gst_element_factory_find(hw);
        if (factory) {
            gst_object_unref(factory);
            if (out_hardware)
                *out_hardware = true;
            return hw;
        }
        if (request_hardware)
            return NULL;
    }

    return sw;
}

static GstElement *build_v4l2src_pipeline(sbs_worker_config_t *config)
{
    GstElement *pipeline = gst_pipeline_new("source-pipeline");
    GstElement *src      = gst_element_factory_make("v4l2src", "src");
    GstElement *src_capsf = gst_element_factory_make("capsfilter", "source-caps");
    GstElement *parser   = NULL;
    GstElement *decoder  = NULL;
    GstElement *conv     = NULL;
    GstElement *scale    = NULL;
    GstElement *out_capsf = NULL;
    GstElement *sink     = gst_element_factory_make("appsink", "sink");
    const char *format = config->source.format;
    bool compressed = v4l2_format_is_mjpeg(format) || v4l2_format_is_h264(format);
    bool direct_import = !compressed && v4l2_raw_format_direct_importable(format);
    bool hardware_decode = false;
    gint fps_num = 30;
    gint fps_den = 1;

    if (compressed) {
        const char *decoder_name = v4l2_decoder_factory_name(format, config->source.decode_mode,
                                                             &hardware_decode);
        if (!decoder_name) {
            LOG_E("hardware decode requested for %s but no compatible decoder is available",
                  format ? format : "compressed V4L2");
        }
        if (v4l2_format_is_h264(format))
            parser = gst_element_factory_make("h264parse", "h264parse");
        decoder = decoder_name ? gst_element_factory_make(decoder_name, "decoder") : NULL;
        conv = gst_element_factory_make("videoconvert", "convert");
        scale = gst_element_factory_make("videoscale", "scale");
        out_capsf = gst_element_factory_make("capsfilter", "output-caps");
    } else if (!direct_import) {
        conv = gst_element_factory_make("videoconvert", "convert");
        scale = gst_element_factory_make("videoscale", "scale");
        out_capsf = gst_element_factory_make("capsfilter", "output-caps");
    }

    if (!pipeline || !src || !src_capsf || !sink ||
        (compressed && (!decoder || !conv || !scale || !out_capsf || (v4l2_format_is_h264(format) && !parser))) ||
        (!compressed && !direct_import && (!conv || !scale || !out_capsf))) {
        LOG_E("failed to create v4l2src pipeline elements");
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (src_capsf) gst_object_unref(src_capsf);
        if (parser) gst_object_unref(parser);
        if (decoder) gst_object_unref(decoder);
        if (conv) gst_object_unref(conv);
        if (scale) gst_object_unref(scale);
        if (out_capsf) gst_object_unref(out_capsf);
        if (sink) gst_object_unref(sink);
        return NULL;
    }

    if (config->source.device_path) {
        g_object_set(src, "device", config->source.device_path, NULL);
    }
    g_object_set(src, "io-mode", direct_import ? 4 /* dmabuf */ : 2 /* mmap */,
                 "do-timestamp", TRUE, NULL);

    parse_framerate_string(config->source.framerate,
                           (gint)config->framerate_num,
                           (gint)config->framerate_den,
                           &fps_num, &fps_den);

    GstCaps *source_caps = compressed
        ? v4l2_compressed_caps(format, config->width, config->height, fps_num, fps_den)
        : v4l2_raw_caps(format, config->width, config->height, fps_num, fps_den);
    g_object_set(src_capsf, "caps", source_caps, NULL);
    gst_caps_unref(source_caps);

    if (!direct_import || compressed) {
        GstCaps *out_caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV21",
            "width", G_TYPE_INT, (gint)config->width,
            "height", G_TYPE_INT, (gint)config->height,
            "framerate", GST_TYPE_FRACTION, fps_num, fps_den,
            NULL);
        g_object_set(out_capsf, "caps", out_caps, NULL);
        gst_caps_unref(out_caps);
    }

    g_object_set(sink,
        "emit-signals", TRUE,
        "max-buffers",  2,
        "drop",         TRUE,
        "sync",         FALSE,
        NULL);

    if (compressed) {
        LOG_I("v4l2src %s decode path: device=%s format=%s -> NV21 %ux%u@%d/%d",
              hardware_decode ? "hardware" : "software",
              config->source.device_path ? config->source.device_path : "default",
              format ? format : "device-default", config->width, config->height,
              fps_num, fps_den);
        if (parser) {
            gst_bin_add_many(GST_BIN(pipeline), src, src_capsf, parser, decoder, conv, scale, out_capsf, sink, NULL);
            if (!gst_element_link_many(src, src_capsf, parser, decoder, conv, scale, out_capsf, sink, NULL)) {
                LOG_E("failed to link h264 %s decode v4l2src pipeline",
                      hardware_decode ? "hardware" : "software");
                gst_object_unref(pipeline);
                return NULL;
            }
        } else {
            gst_bin_add_many(GST_BIN(pipeline), src, src_capsf, decoder, conv, scale, out_capsf, sink, NULL);
            if (!gst_element_link_many(src, src_capsf, decoder, conv, scale, out_capsf, sink, NULL)) {
                LOG_E("failed to link mjpeg %s decode v4l2src pipeline",
                      hardware_decode ? "hardware" : "software");
                gst_object_unref(pipeline);
                return NULL;
            }
        }
    } else if (direct_import) {
        LOG_I("v4l2src direct raw DMA-BUF path: device=%s format=%s %ux%u@%d/%d",
              config->source.device_path ? config->source.device_path : "default",
              format ? format : "device-default", config->width, config->height,
              fps_num, fps_den);
        gst_bin_add_many(GST_BIN(pipeline), src, src_capsf, sink, NULL);
        if (!gst_element_link_many(src, src_capsf, sink, NULL)) {
            LOG_E("failed to link direct v4l2src pipeline");
            gst_object_unref(pipeline);
            return NULL;
        }
    } else {
        LOG_I("v4l2src raw fallback path: device=%s format=%s -> NV21 %ux%u@%d/%d",
              config->source.device_path ? config->source.device_path : "default",
              format ? format : "device-default", config->width, config->height,
              fps_num, fps_den);
        gst_bin_add_many(GST_BIN(pipeline), src, src_capsf, conv, scale, out_capsf, sink, NULL);
        if (!gst_element_link_many(src, src_capsf, conv, scale, out_capsf, sink, NULL)) {
            LOG_E("failed to link fallback v4l2src pipeline");
            gst_object_unref(pipeline);
            return NULL;
        }
    }

    return pipeline;
}

/* ── CPU Buffer → DMA-BUF Export ──────────────────────────────── */

/**
 * Export a CPU-mapped GstBuffer as a memfd.
 *
 * For sources like videotestsrc that produce CPU-only buffers,
 * we copy the frame data into a DMA-BUF (preferred, needed for GE2D)
 * or memfd (fallback) and send the fd over IPC.
 *
 * @return fd on success, -1 on failure
 */
static int export_cpu_buffer(GstBuffer *buffer, source_state_t *state,
                             bool *out_is_dmabuf)
{
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        LOG_E("failed to map GstBuffer for CPU export");
        return -1;
    }

    *out_is_dmabuf = false;

    /* Try DMA-BUF allocation first — required for GE2D hardware compositor */
    if (state->dmabuf_available) {
        void *dst = NULL;
        sbs_dmabuf_buffer_t *dbuf = NULL;
        int export_fd = source_worker_acquire_cpu_export_slot(state, map.size, &dst, &dbuf);
        if (export_fd >= 0 && dst && dbuf) {
            sbs_dmabuf_alloc_sync(dbuf, true, SBS_DMABUF_SYNC_WRITE);
            memcpy(dst, map.data, map.size);
            sbs_dmabuf_alloc_sync(dbuf, false, SBS_DMABUF_SYNC_WRITE);
            gst_buffer_unmap(buffer, &map);
            *out_is_dmabuf = true;
            return export_fd;
        }
    }

    /* Fallback: memfd (will NOT work with GE2D, but keeps Vulkan path alive) */
    int fd = memfd_create("sbs-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        LOG_E("memfd_create failed: %s", strerror(errno));
        gst_buffer_unmap(buffer, &map);
        return -1;
    }

    if (ftruncate(fd, (off_t)map.size) < 0) {
        LOG_E("ftruncate memfd failed: %s", strerror(errno));
        close(fd);
        gst_buffer_unmap(buffer, &map);
        return -1;
    }

    ssize_t written = 0;
    while ((size_t)written < map.size) {
        ssize_t n = write(fd, map.data + written, map.size - (size_t)written);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_E("write to memfd failed: %s", strerror(errno));
            close(fd);
            gst_buffer_unmap(buffer, &map);
            return -1;
        }
        written += n;
    }

    gst_buffer_unmap(buffer, &map);
    return fd;
}

static int export_linear_nv21(GstBuffer *buffer, GstCaps *caps,
                              sbs_video_frame_msg_t *msg,
                              source_state_t *state,
                              bool *out_is_dmabuf)
{
    GstVideoInfo vinfo;
    GstVideoFrame frame;
    int fd = -1;
    size_t total_size;
    uint8_t *dst;

    *out_is_dmabuf = false;

    if (!caps || !msg || !gst_video_info_from_caps(&vinfo, caps)) {
        return -1;
    }
    if (GST_VIDEO_INFO_FORMAT(&vinfo) != GST_VIDEO_FORMAT_NV21) {
        return -1;
    }
    if (!gst_video_frame_map(&frame, &vinfo, buffer, GST_MAP_READ)) {
        return -1;
    }

    total_size = (size_t)GST_VIDEO_INFO_WIDTH(&vinfo) * (size_t)GST_VIDEO_INFO_HEIGHT(&vinfo) * 3 / 2;

    /* Try DMA-BUF allocation first — required for GE2D hardware compositor */
    sbs_dmabuf_buffer_t *dbuf = NULL;
    if (state->dmabuf_available) {
        fd = source_worker_acquire_cpu_export_slot(state, total_size, (void **)&dst, &dbuf);
        if (fd >= 0 && dst && dbuf) {
            *out_is_dmabuf = true;
        }
    }

    /* Fallback: memfd */
    if (fd < 0) {
        fd = memfd_create("sbs-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) {
            gst_video_frame_unmap(&frame);
            return -1;
        }
        if (ftruncate(fd, (off_t)total_size) < 0) {
            close(fd);
            gst_video_frame_unmap(&frame);
            return -1;
        }
    }

    if (!*out_is_dmabuf) {
        dst = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (dst == MAP_FAILED) {
            close(fd);
            gst_video_frame_unmap(&frame);
            return -1;
        }
    }

    if (*out_is_dmabuf)
        sbs_dmabuf_alloc_sync(dbuf, true, SBS_DMABUF_SYNC_WRITE);

    {
        uint32_t width = (uint32_t)GST_VIDEO_INFO_WIDTH(&vinfo);
        uint32_t height = (uint32_t)GST_VIDEO_INFO_HEIGHT(&vinfo);
        const uint8_t *y_src = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        const uint8_t *vu_src = GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
        gint y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        gint vu_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
        uint8_t *y_dst = dst;
        uint8_t *vu_dst = dst + (width * height);

        for (uint32_t y = 0; y < height; y++) {
            memcpy(y_dst + (y * width), y_src + ((gint)y * y_stride), width);
        }
        for (uint32_t y = 0; y < height / 2; y++) {
            memcpy(vu_dst + (y * width), vu_src + ((gint)y * vu_stride), width);
        }

        msg->width = width;
        msg->height = height;
        msg->n_planes = 2;
        msg->plane_offset[0] = 0;
        msg->plane_offset[1] = width * height;
        msg->plane_stride[0] = width;
        msg->plane_stride[1] = width;
    }

    if (*out_is_dmabuf)
        sbs_dmabuf_alloc_sync(dbuf, false, SBS_DMABUF_SYNC_WRITE);

    if (!*out_is_dmabuf)
        munmap(dst, total_size);
    gst_video_frame_unmap(&frame);
    return fd;
}

/* ── Appsink Callback ─────────────────────────────────────────── */

/**
 * Extract video info from GstCaps to fill frame message fields.
 */
static void fill_frame_from_caps(sbs_video_frame_msg_t *msg, GstCaps *caps)
{
    GstVideoInfo vinfo;
    if (caps && gst_video_info_from_caps(&vinfo, caps)) {
        msg->width  = (uint32_t)GST_VIDEO_INFO_WIDTH(&vinfo);
        msg->height = (uint32_t)GST_VIDEO_INFO_HEIGHT(&vinfo);
        msg->n_planes = (uint32_t)GST_VIDEO_INFO_N_PLANES(&vinfo);

        for (uint32_t i = 0; i < msg->n_planes && i < 4; i++) {
            msg->plane_offset[i] = (uint32_t)GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, i);
            msg->plane_stride[i] = (uint32_t)GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, i);
        }

        GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&vinfo);
        switch (fmt) {
        case GST_VIDEO_FORMAT_NV12:
            msg->drm_format = DRM_FORMAT_NV12;
            break;
        case GST_VIDEO_FORMAT_NV21:
            msg->drm_format = DRM_FORMAT_NV21;
            break;
        case GST_VIDEO_FORMAT_P010_10LE:
        case GST_VIDEO_FORMAT_P010_10BE:
            msg->drm_format = DRM_FORMAT_P010;
            break;
        case GST_VIDEO_FORMAT_RGBA:
            msg->drm_format = DRM_FORMAT_ABGR8888;
            break;
        default:
            msg->drm_format = 0;
            break;
        }
        {
            static uint32_t caps_log_counter = 0;
            if (caps_log_counter++ % 60 == 0) {
                LOG_I("fill_frame_from_caps: fmt=%s drm_format=%.4s width=%u height=%u n_planes=%u stride0=%u stride1=%u offset0=%u offset1=%u",
                      gst_video_format_to_string(fmt),
                      (const char *)&msg->drm_format,
                      msg->width, msg->height, msg->n_planes,
                      msg->plane_stride[0], msg->plane_stride[1],
                      msg->plane_offset[0], msg->plane_offset[1]);
            }
        }
    } else {
        LOG_W("fill_frame_from_caps: gst_video_info_from_caps failed");
    }
}

/**
 * Appsink new-sample callback.
 *
 * Called from the streaming thread when a new frame is available.
 * Extracts the DMA-BUF fd (preferred for GE2D hardware compositor)
 * or exports CPU buffer to DMA-BUF/memfd, builds a video frame message,
 * and sends it to the supervisor via IPC.
 */
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;

    if (state->ctx->shutting_down) {
        return GST_FLOW_EOS;
    }

    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    int dmabuf_fd = -1;
    bool is_dmabuf = false;
    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    const char *source_type = state->ctx->config.source.source_type;
    bool compressed_v4l2 = source_type && strcmp(source_type, "v4l2src") == 0 &&
        (v4l2_format_is_mjpeg(state->ctx->config.source.format) ||
         v4l2_format_is_h264(state->ctx->config.source.format));
    bool force_cpu_export = source_type && strcmp(source_type, "v4l2src") == 0;

    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
    msg.pts_ns      = GST_BUFFER_PTS(buffer);
    msg.dts_ns      = GST_BUFFER_DTS(buffer);
    msg.duration_ns = GST_BUFFER_DURATION(buffer);
    msg.sequence    = state->frame_counter;
    msg.width       = state->ctx->config.width;
    msg.height      = state->ctx->config.height;

    if (compressed_v4l2) {
        /* Hardware decoders may output cropped or tiled DMA-BUFs that are not
         * directly importable as tight NV21; normalize decoded frames first. */
        dmabuf_fd = export_linear_nv21(buffer, caps, &msg, state, &is_dmabuf);
        if (dmabuf_fd < 0) {
            dmabuf_fd = export_cpu_buffer(buffer, state, &is_dmabuf);
        }
    } else if (gst_is_dmabuf_memory(mem)) {
        /* Native DMA-BUF — use directly regardless of source type */
        int orig_fd = gst_dmabuf_memory_get_fd(mem);
        dmabuf_fd = dup(orig_fd);
        if (dmabuf_fd < 0) {
            LOG_W("dup(dmabuf_fd=%d) failed: %s", orig_fd, strerror(errno));
        }
        is_dmabuf = true;
    } else if (force_cpu_export) {
        dmabuf_fd = export_linear_nv21(buffer, caps, &msg, state, &is_dmabuf);
        if (dmabuf_fd < 0) {
            dmabuf_fd = export_cpu_buffer(buffer, state, &is_dmabuf);
        }
    } else {
        /* CPU buffer — export as DMA-BUF (preferred) or memfd (fallback) */
        dmabuf_fd = export_cpu_buffer(buffer, state, &is_dmabuf);
    }

    if (dmabuf_fd < 0) {
        LOG_W("failed to export frame %lu, skipping", (unsigned long)state->frame_counter);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* Fill detailed plane info from caps since source resolution may differ
     * from the canvas config. */
    fill_frame_from_caps(&msg, caps);

    if (msg.width != state->ctx->config.width || msg.height != state->ctx->config.height) {
        if (state->frame_counter == 0 || state->frame_counter % 300 == 0) {
            LOG_I("caps resolution %ux%u differs from config %ux%u",
                  msg.width, msg.height,
                  state->ctx->config.width, state->ctx->config.height);
        }
    }

    /* Set DRM modifier to LINEAR for all sources.
     * TODO: query actual modifier from DMA-BUF metadata when available. */
    msg.drm_modifier = 0;  /* DRM_FORMAT_MOD_LINEAR */

    /* Set buffer type so compositor knows whether GE2D can use this fd */
    msg.buffer_type = is_dmabuf ? SBS_FRAME_BUFFER_DMABUF : SBS_FRAME_BUFFER_MEMFD;

    /* Log buffer type on first frame, and every 60 frames thereafter */
    if (state->frame_counter == 0 || state->frame_counter % 60 == 0) {
        LOG_I("frame %lu exported as %s (fd=%d, %ux%u)",
              (unsigned long)state->frame_counter,
              is_dmabuf ? "DMA-BUF" : "memfd",
              dmabuf_fd, msg.width, msg.height);
    }

    /* Flag CPU-exported frames that are not DMA-BUF */
    if (!is_dmabuf && !gst_is_dmabuf_memory(mem)) {
        msg.flags |= SBS_FRAME_FLAG_CORRUPTED;  /* Reusing flag to indicate CPU buffer */
    }

    /* Send frame + fd over IPC */
    int rc = sbs_ipc_send_frame(state->ctx->sock_fd, &msg, dmabuf_fd);
    if (rc == SBS_ERR_WOULD_BLOCK) {
        /* Supervisor is busy — drop this frame */
        state->frames_dropped++;
        LOG_T("frame %lu dropped (WOULD_BLOCK)", (unsigned long)state->frame_counter);
    } else if (rc == SBS_ERR_PIPE) {
        LOG_W("send_frame: broken pipe — supervisor disconnected, stopping");
        close(dmabuf_fd);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    } else if (rc != SBS_OK) {
        LOG_W("send_frame failed: %d", rc);
    }

    close(dmabuf_fd);
    state->frame_counter++;
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

static gboolean on_text_frame_timeout(gpointer user_data)
{
    source_state_t *state = user_data;
    sbs_worker_config_t *config;
    uint32_t width, height;
    size_t total_size;
    void *dst = NULL;
    sbs_dmabuf_buffer_t *dbuf = NULL;
    int fd = -1;
    bool is_dmabuf = false;
    bool mapped_fallback = false;

    if (!state || state->ctx->shutting_down)
        return G_SOURCE_REMOVE;

    config = &state->ctx->config;
    width = config->width > 0 ? config->width : 1280u;
    height = config->height > 0 ? config->height : 256u;
    total_size = (size_t)width * (size_t)height * 4u;

    if (state->dmabuf_available) {
        fd = source_worker_acquire_cpu_export_slot(state, total_size, &dst, &dbuf);
        if (fd >= 0 && dst && dbuf) {
            is_dmabuf = true;
            sbs_dmabuf_alloc_sync(dbuf, true, SBS_DMABUF_SYNC_WRITE);
        }
    }

    if (fd < 0) {
        fd = memfd_create("sbs-text-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0 || ftruncate(fd, (off_t)total_size) < 0) {
            if (fd >= 0) close(fd);
            state->frames_dropped++;
            return G_SOURCE_CONTINUE;
        }
        dst = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (dst == MAP_FAILED) {
            close(fd);
            state->frames_dropped++;
            return G_SOURCE_CONTINUE;
        }
        mapped_fallback = true;
    }

    if (render_text_rgba(config, dst, width, height) != 0) {
        if (is_dmabuf)
            sbs_dmabuf_alloc_sync(dbuf, false, SBS_DMABUF_SYNC_WRITE);
        if (mapped_fallback)
            munmap(dst, total_size);
        close(fd);
        state->frames_dropped++;
        return G_SOURCE_CONTINUE;
    }

    if (is_dmabuf)
        sbs_dmabuf_alloc_sync(dbuf, false, SBS_DMABUF_SYNC_WRITE);
    if (mapped_fallback)
        munmap(dst, total_size);

    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
    msg.pts_ns = (uint64_t)g_get_monotonic_time() * 1000ull;
    msg.dts_ns = msg.pts_ns;
    msg.duration_ns = 1000000000ull / MAX(config->framerate_num, 1u);
    msg.sequence = state->frame_counter;
    msg.width = width;
    msg.height = height;
    msg.drm_format = DRM_FORMAT_ABGR8888;
    msg.drm_modifier = 0;
    msg.buffer_type = is_dmabuf ? SBS_FRAME_BUFFER_DMABUF : SBS_FRAME_BUFFER_MEMFD;
    msg.n_planes = 1;
    msg.plane_offset[0] = 0;
    msg.plane_stride[0] = width * 4u;

    int rc = sbs_ipc_send_frame(state->ctx->sock_fd, &msg, fd);
    if (rc == SBS_ERR_WOULD_BLOCK) {
        state->frames_dropped++;
    } else if (rc == SBS_ERR_PIPE) {
        close(fd);
        return G_SOURCE_REMOVE;
    } else if (rc != SBS_OK) {
        LOG_W("text frame send failed: %d", rc);
        state->frames_dropped++;
    } else if (state->frame_counter == 0 || state->frame_counter % 60 == 0) {
        LOG_I("text frame %lu exported as %s (%ux%u font=%s size=%u)",
              (unsigned long)state->frame_counter,
              is_dmabuf ? "DMA-BUF" : "memfd",
              width, height,
              config->source.font_family && config->source.font_family[0]
                  ? config->source.font_family : "Liberation Sans",
              config->source.font_size > 0 ? config->source.font_size : 72u);
    }

    close(fd);
    state->frame_counter++;
    return G_SOURCE_CONTINUE;
}

/* ── Bus Message Handler ──────────────────────────────────────── */

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *s = gst_message_get_structure(msg);
        if (gst_structure_has_name(s, "hdmi-signal-change")) {
            LOG_I("HDMI signal change detected");

            /* Build signal change message */
            sbs_signal_change_msg_t sc;
            sbs_signal_change_msg_init(&sc);

            const gchar *reason = gst_structure_get_string(s, "reason");
            if (reason) {
                strncpy(sc.reason, reason, sizeof(sc.reason) - 1);
            }

            gst_structure_get_uint(s, "width",       &sc.width);
            gst_structure_get_uint(s, "height",      &sc.height);
            gst_structure_get_uint(s, "frame-rate",  &sc.frame_rate_raw);
            gst_structure_get_uint(s, "color-depth", &sc.color_depth);
            gst_structure_get_uint(s, "dolby-vision", &sc.dolby_vision);
            gst_structure_get_uint(s, "interlace",   &sc.interlace);

            const gchar *cs = gst_structure_get_string(s, "color-space");
            if (cs) strncpy(sc.color_space, cs, sizeof(sc.color_space) - 1);

            const gchar *eotf = gst_structure_get_string(s, "hdr-eotf");
            if (eotf) strncpy(sc.hdr_eotf, eotf, sizeof(sc.hdr_eotf) - 1);

            /* Send to supervisor */
            sbs_ipc_send_msg(state->ctx->sock_fd, &sc, sizeof(sc));

            LOG_I("signal change: %s (%ux%u, %s, %s)",
                  sc.reason, sc.width, sc.height,
                  sc.color_space, sc.hdr_eotf);

            /* Exit cleanly — supervisor will restart with new params */
            sbs_worker_request_shutdown(state->ctx);
        }
        break;
    }

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
        if (state->ctx->config.source.loop) {
            LOG_I("end of stream, seeking to start");
            if (gst_element_seek_simple(state->pipeline, GST_FORMAT_TIME,
                                        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                        0)) {
                break;
            }
            LOG_W("loop seek failed, stopping source");
        }
        LOG_I("end of stream");
        sbs_worker_send_status(state->ctx, SBS_WORKER_STATE_EOS);
        sbs_worker_request_shutdown(state->ctx);
        break;

    case GST_MESSAGE_STATE_CHANGED:
        /* Only log pipeline-level state changes */
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
    source_state_t *state = (source_state_t *)user_data;

    if (state->ctx->shutting_down) {
        return G_SOURCE_REMOVE;
    }

    sbs_status_msg_t status;
    sbs_status_msg_init(&status);
    status.state = (uint32_t)SBS_WORKER_STATE_RUNNING;
    status.frame_width  = state->ctx->config.width;
    status.frame_height = state->ctx->config.height;
    status.frame_rate_num = state->ctx->config.framerate_num;
    status.frame_rate_den = state->ctx->config.framerate_den;
    status.frames_produced = state->frame_counter;
    status.frames_dropped  = state->frames_dropped;

    int rc = sbs_ipc_send_msg(state->ctx->sock_fd, &status, sizeof(status));
    if (rc != SBS_OK && rc != SBS_ERR_WOULD_BLOCK) {
        LOG_W("heartbeat send failed: %d", rc);
    }

    LOG_D("heartbeat: %lu frames, %lu dropped",
          (unsigned long)state->frame_counter,
          (unsigned long)state->frames_dropped);

    return G_SOURCE_CONTINUE;
}

/* ── VFMcap Helpers ───────────────────────────────────────────── */

static gboolean on_vfmcap_frame_timeout(gpointer user_data);
static gboolean on_vfmcap_event_timeout(gpointer user_data);
static gboolean source_worker_vfmcap_recover_timeout(gpointer user_data);
static void source_worker_vfmcap_begin_recovery(source_state_t *state,
                                                const char *reason);
static bool source_worker_vfmcap_restart_now(source_state_t *state,
                                             const char *reason);

static vfmcap_output_fmt_t vfmcap_output_fmt_from_string(const char *s)
{
    if (!s || strcmp(s, "auto") == 0) return VFMCAP_FMT_RAW;
    if (strcmp(s, "raw") == 0 || strcmp(s, "amly") == 0) return VFMCAP_FMT_RAW;
    LOG_W("vfmcap output_format '%s' ignored; SBS compositor now owns raw AMLY conversion", s);
    return VFMCAP_FMT_RAW;
}

static const char *vfmcap_output_fmt_name(vfmcap_output_fmt_t fmt)
{
    if (fmt == VFMCAP_FMT_RAW) return "raw";
    return fmt == VFMCAP_FMT_P010 ? "p010" : "nv12";
}

static void fill_signal_colorimetry(const vfmcap_signal_info_t *info,
                                    char *color_space, size_t color_space_size,
                                    char *hdr_eotf, size_t hdr_eotf_size)
{
    const char *space = "BT.709";
    const char *eotf = "SDR";

    if (info) {
        if (info->hdr_status == 1 || info->hdr_status == 2 || info->hdr_status == 3) {
            space = "BT.2020";
        }
        if (info->hdr_status == 1 || info->hdr_status == 3) {
            eotf = "PQ";
        } else if (info->hdr_status == 2) {
            eotf = "HLG";
        }
    }

    g_strlcpy(color_space, space, color_space_size);
    g_strlcpy(hdr_eotf, eotf, hdr_eotf_size);
}

static void send_vfmcap_signal_info(source_state_t *state,
                                    const char *reason,
                                    const vfmcap_signal_info_t *info)
{
    sbs_signal_change_msg_t sc;
    sbs_signal_change_msg_init(&sc);
    g_strlcpy(sc.reason, reason ? reason : "signal-change", sizeof(sc.reason));
    if (info) {
        sc.width = info->width;
        sc.height = info->height;
        sc.frame_rate_raw = info->fps / 1000;
        sc.color_depth = info->bitdepth;
        sc.dolby_vision = info->hdr_status == 4 ? 1 : 0;
        sc.interlace = info->is_interlaced;
        fill_signal_colorimetry(info, sc.color_space, sizeof(sc.color_space),
                                sc.hdr_eotf, sizeof(sc.hdr_eotf));
    }
    sbs_ipc_send_msg(state->ctx->sock_fd, &sc, sizeof(sc));
}

static uint32_t source_worker_vfmcap_lease_count(source_state_t *state)
{
    uint32_t count = 0;
    if (!state)
        return 0;
    for (uint32_t i = 0; i < SBS_VFMCAP_MAX_IN_FLIGHT; i++) {
        if (state->vfmcap_leases[i].active)
            count++;
    }
    return count;
}

static void source_worker_vfmcap_note_lease_depth(source_state_t *state,
                                                  uint32_t count)
{
    if (state && count > state->vfmcap_max_in_flight)
        state->vfmcap_max_in_flight = count;
}

static void source_worker_vfmcap_reset_stats_window(source_state_t *state,
                                                    int64_t now_us,
                                                    uint32_t current_leases)
{
    if (!state)
        return;

    state->vfmcap_stats_last_us = now_us;
    state->vfmcap_stats_last_frame_counter = state->frame_counter;
    state->vfmcap_stats_last_drop_counter = state->frames_dropped;
    state->vfmcap_acquire_attempts = 0;
    state->vfmcap_acquire_ok = 0;
    state->vfmcap_acquire_timeouts = 0;
    state->vfmcap_acquire_nosig = 0;
    state->vfmcap_acquire_errors = 0;
    state->vfmcap_acquire_reconfigured = 0;
    state->vfmcap_acquire_total_us = 0;
    state->vfmcap_acquire_max_us = 0;
    state->vfmcap_success_intervals = 0;
    state->vfmcap_success_interval_total_us = 0;
    state->vfmcap_success_interval_max_us = 0;
    state->vfmcap_no_lease_skips = 0;
    state->vfmcap_no_fd_drops = 0;
    state->vfmcap_split_plane_drops = 0;
    state->vfmcap_lease_full_drops = 0;
    state->vfmcap_send_would_block = 0;
    state->vfmcap_send_errors = 0;
    state->vfmcap_release_acks = 0;
    state->vfmcap_unknown_release_acks = 0;
    state->vfmcap_max_in_flight = current_leases;
}

static gboolean on_vfmcap_stats_timeout(gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;
    if (!state || state->ctx->shutting_down)
        return G_SOURCE_REMOVE;

    int64_t now_us = g_get_monotonic_time();
    int64_t elapsed_us = state->vfmcap_stats_last_us > 0 ?
        now_us - state->vfmcap_stats_last_us : 0;
    if (elapsed_us <= 0)
        elapsed_us = 1;

    uint64_t sent = state->frame_counter - state->vfmcap_stats_last_frame_counter;
    uint64_t dropped = state->frames_dropped - state->vfmcap_stats_last_drop_counter;
    uint32_t leases = source_worker_vfmcap_lease_count(state);
    source_worker_vfmcap_note_lease_depth(state, leases);

    double seconds = (double)elapsed_us / 1000000.0;
    double fps = seconds > 0.0 ? (double)sent / seconds : 0.0;
    double avg_acquire_ms = state->vfmcap_acquire_attempts > 0 ?
        (double)state->vfmcap_acquire_total_us /
        (double)state->vfmcap_acquire_attempts / 1000.0 : 0.0;
    double max_acquire_ms = (double)state->vfmcap_acquire_max_us / 1000.0;
    double avg_gap_ms = state->vfmcap_success_intervals > 0 ?
        (double)state->vfmcap_success_interval_total_us /
        (double)state->vfmcap_success_intervals / 1000.0 : 0.0;
    double max_gap_ms = (double)state->vfmcap_success_interval_max_us / 1000.0;
    double expected_fps = 0.0;

    if (state->vfmcap_ctx) {
        vfmcap_signal_info_t info = {0};
        if (vfmcap_get_signal_info(state->vfmcap_ctx, &info) == VFMCAP_OK && info.fps > 0) {
            expected_fps = (double)info.fps;
            if (expected_fps > 1000.0 && expected_fps < 12000.0)
                expected_fps /= 100.0;
            else if (expected_fps > 120000.0)
                expected_fps /= 1000.0;
        }
    }
    if (expected_fps < 45.0 && state->ctx->config.framerate_num > 0) {
        uint32_t den = state->ctx->config.framerate_den > 0
            ? state->ctx->config.framerate_den : 1u;
        expected_fps = (double)state->ctx->config.framerate_num / (double)den;
    }

    LOG_I("vfmcap stats: sent=%lu %.1ffps expected=%.1f low=%u dropped=%lu leases=%u/%u max=%u "
          "acq=%lu ok=%lu timeout=%lu nosig=%lu err=%lu reconfig=%lu "
          "acq_ms=%.2f/%.2f gap_ms=%.2f/%.2f release=%lu unknown_release=%lu "
          "send_block=%lu send_err=%lu no_lease=%lu no_fd=%lu split=%lu lease_full=%lu",
          (unsigned long)sent, fps, expected_fps, state->vfmcap_low_fps_windows,
          (unsigned long)dropped,
          leases, SBS_VFMCAP_MAX_IN_FLIGHT, state->vfmcap_max_in_flight,
          (unsigned long)state->vfmcap_acquire_attempts,
          (unsigned long)state->vfmcap_acquire_ok,
          (unsigned long)state->vfmcap_acquire_timeouts,
          (unsigned long)state->vfmcap_acquire_nosig,
          (unsigned long)state->vfmcap_acquire_errors,
          (unsigned long)state->vfmcap_acquire_reconfigured,
          avg_acquire_ms, max_acquire_ms, avg_gap_ms, max_gap_ms,
          (unsigned long)state->vfmcap_release_acks,
          (unsigned long)state->vfmcap_unknown_release_acks,
          (unsigned long)state->vfmcap_send_would_block,
          (unsigned long)state->vfmcap_send_errors,
          (unsigned long)state->vfmcap_no_lease_skips,
          (unsigned long)state->vfmcap_no_fd_drops,
          (unsigned long)state->vfmcap_split_plane_drops,
          (unsigned long)state->vfmcap_lease_full_drops);

    if (expected_fps >= 45.0 && fps < expected_fps * 0.75 &&
        leases < SBS_VFMCAP_MAX_IN_FLIGHT / 2 &&
        state->vfmcap_send_would_block == 0 &&
        state->vfmcap_send_errors == 0 &&
        state->vfmcap_lease_full_drops == 0) {
        state->vfmcap_low_fps_windows++;
        if (state->vfmcap_low_fps_windows >= SBS_VFMCAP_LOW_FPS_RECOVER_WINDOWS) {
            char reason[96];
            snprintf(reason, sizeof(reason), "low capture fps %.1f/%.1f",
                     fps, expected_fps);
            source_worker_vfmcap_restart_now(state, reason);
            return G_SOURCE_CONTINUE;
        }
    } else {
        state->vfmcap_low_fps_windows = 0;
    }

    source_worker_vfmcap_reset_stats_window(state, now_us, leases);
    return G_SOURCE_CONTINUE;
}

static int source_worker_vfmcap_store_lease(source_state_t *state,
                                            uint64_t sequence,
                                            const vfmcap_frame_t *frame)
{
    if (!state || !frame)
        return -1;
    for (uint32_t i = 0; i < SBS_VFMCAP_MAX_IN_FLIGHT; i++) {
        if (!state->vfmcap_leases[i].active) {
            state->vfmcap_leases[i].active = true;
            state->vfmcap_leases[i].sequence = sequence;
            state->vfmcap_leases[i].frame = *frame;
            source_worker_vfmcap_note_lease_depth(state,
                source_worker_vfmcap_lease_count(state));
            return (int)i;
        }
    }
    return -1;
}

static void source_worker_vfmcap_release_lease_index(source_state_t *state,
                                                     uint32_t idx)
{
    if (!state || idx >= SBS_VFMCAP_MAX_IN_FLIGHT)
        return;
    source_vfmcap_lease_t *lease = &state->vfmcap_leases[idx];
    if (!lease->active)
        return;
    if (!state->vfmcap_ctx) {
        memset(lease, 0, sizeof(*lease));
        return;
    }
    vfmcap_release_frame(state->vfmcap_ctx, &lease->frame);
    memset(lease, 0, sizeof(*lease));
}

static void source_worker_vfmcap_release_sequence(source_state_t *state,
                                                  uint64_t sequence)
{
    if (!state)
        return;
    for (uint32_t i = 0; i < SBS_VFMCAP_MAX_IN_FLIGHT; i++) {
        if (state->vfmcap_leases[i].active &&
            state->vfmcap_leases[i].sequence == sequence) {
            state->vfmcap_release_acks++;
            source_worker_vfmcap_release_lease_index(state, i);
            return;
        }
    }
    state->vfmcap_unknown_release_acks++;
    LOG_D("vfmcap release for unknown frame sequence=%lu", (unsigned long)sequence);
}

static void source_worker_vfmcap_release_all(source_state_t *state)
{
    if (!state)
        return;
    for (uint32_t i = 0; i < SBS_VFMCAP_MAX_IN_FLIGHT; i++)
        source_worker_vfmcap_release_lease_index(state, i);
}

static bool source_worker_vfmcap_signal_info_path(const source_state_t *state,
                                                  char *path,
                                                  size_t path_size)
{
    const char *device;
    char resolved[PATH_MAX];
    const char *node;

    if (!path || path_size == 0)
        return false;

    device = state && state->ctx && state->ctx->config.source.device_path &&
        state->ctx->config.source.device_path[0]
        ? state->ctx->config.source.device_path
        : SBS_VFMCAP_DEFAULT_DEVICE;

    if (!realpath(device, resolved))
        g_strlcpy(resolved, device, sizeof(resolved));

    node = strrchr(resolved, '/');
    node = node ? node + 1 : resolved;
    if (strncmp(node, "video", 5) != 0 || node[5] == '\0')
        return false;

    g_snprintf(path, path_size, "/sys/class/video4linux/%s/signal_info", node);
    return true;
}

static bool source_worker_read_signal_resolution(source_state_t *state,
                                                 uint32_t *width,
                                                 uint32_t *height)
{
    gchar *contents = NULL;
    char signal_path[PATH_MAX];
    uint32_t w = 0;
    uint32_t h = 0;

    if (!width || !height)
        return false;

    if (!source_worker_vfmcap_signal_info_path(state, signal_path, sizeof(signal_path)) ||
        !g_file_get_contents(signal_path, &contents, NULL, NULL))
        return false;

    char *p = strstr(contents, "width:");
    if (p)
        (void)sscanf(p, "width: %u", &w);
    p = strstr(contents, "height:");
    if (p)
        (void)sscanf(p, "height: %u", &h);

    g_free(contents);
    if (w == 0 || h == 0)
        return false;

    *width = w;
    *height = h;
    return true;
}

static int source_worker_vfmcap_open_start(source_state_t *state)
{
    const char *device;
    int rc;

    if (!state || !state->ctx)
        return SBS_ERR_INVAL;

    device = state->ctx->config.source.device_path;
    if (!device || device[0] == '\0')
        device = SBS_VFMCAP_DEFAULT_DEVICE;

    state->vfmcap_ctx = vfmcap_open(device, &state->vfmcap_config);
    if (!state->vfmcap_ctx) {
        LOG_E("vfmcap_open(%s) failed: %s", device, vfmcap_last_error(NULL));
        return SBS_ERR_IO;
    }

    rc = vfmcap_start(state->vfmcap_ctx, SBS_VFMCAP_BUFFER_COUNT);
    if (rc != VFMCAP_OK) {
        LOG_E("vfmcap_start failed: %d (%s)", rc, vfmcap_last_error(state->vfmcap_ctx));
        vfmcap_close(state->vfmcap_ctx);
        state->vfmcap_ctx = NULL;
        return SBS_ERR_IO;
    }

    {
        vfmcap_signal_info_t info = {0};
        if (vfmcap_get_signal_info(state->vfmcap_ctx, &info) == VFMCAP_OK) {
            LOG_I("vfmcap signal: %ux%u fps=%u bitdepth=%u hdr_status=%u",
                  info.width, info.height, info.fps, info.bitdepth, info.hdr_status);
            send_vfmcap_signal_info(state, "signal-change", &info);
        }
    }

    LOG_I("vfmcap started (fmt=%s color=passthrough target=source)",
          vfmcap_output_fmt_name(state->vfmcap_output_format));
    state->vfmcap_last_success_us = 0;
    state->vfmcap_consecutive_nosig = 0;
    state->vfmcap_low_fps_windows = 0;
    return SBS_OK;
}

static void source_worker_vfmcap_close_current(source_state_t *state)
{
    if (!state || !state->vfmcap_ctx)
        return;

    source_worker_vfmcap_release_all(state);
    vfmcap_stop(state->vfmcap_ctx);
    vfmcap_close(state->vfmcap_ctx);
    state->vfmcap_ctx = NULL;
}

static bool source_worker_vfmcap_restart_now(source_state_t *state,
                                             const char *reason)
{
    if (!state || state->vfmcap_recovering || state->ctx->shutting_down)
        return false;

    LOG_W("vfmcap restarting after %s", reason ? reason : "low capture fps");

    if (state->vfmcap_frame_timer) {
        g_source_remove(state->vfmcap_frame_timer);
        state->vfmcap_frame_timer = 0;
    }
    if (state->vfmcap_event_timer) {
        g_source_remove(state->vfmcap_event_timer);
        state->vfmcap_event_timer = 0;
    }

    source_worker_vfmcap_close_current(state);
    if (source_worker_vfmcap_open_start(state) != SBS_OK) {
        source_worker_vfmcap_begin_recovery(state, reason);
        return false;
    }

    source_worker_vfmcap_reset_stats_window(state, g_get_monotonic_time(), 0);
    state->vfmcap_frame_timer = g_idle_add(on_vfmcap_frame_timeout, state);
    state->vfmcap_event_timer = g_timeout_add(100, on_vfmcap_event_timeout, state);
    sbs_worker_send_status(state->ctx, SBS_WORKER_STATE_RUNNING);
    LOG_I("vfmcap restart complete");
    return true;
}

static void source_worker_vfmcap_begin_recovery(source_state_t *state,
                                                const char *reason)
{
    if (!state || state->vfmcap_recovering || state->ctx->shutting_down)
        return;

    state->vfmcap_recovering = true;
    state->vfmcap_consecutive_nosig = 0;
    state->vfmcap_low_fps_windows = 0;
    state->vfmcap_stable_count = 0;
    state->vfmcap_stable_width = 0;
    state->vfmcap_stable_height = 0;

    LOG_W("vfmcap entering recovery after %s; waiting for stable HDMI signal",
          reason ? reason : "signal loss");
    send_vfmcap_signal_info(state, "signal-lost", NULL);

    if (state->vfmcap_frame_timer) {
        g_source_remove(state->vfmcap_frame_timer);
        state->vfmcap_frame_timer = 0;
    }
    if (state->vfmcap_event_timer) {
        g_source_remove(state->vfmcap_event_timer);
        state->vfmcap_event_timer = 0;
    }

    source_worker_vfmcap_close_current(state);
    source_worker_vfmcap_reset_stats_window(state, g_get_monotonic_time(), 0);

    if (!state->vfmcap_recover_timer) {
        state->vfmcap_recover_timer = g_timeout_add(SBS_VFMCAP_RECOVER_INTERVAL_MS,
                                                    source_worker_vfmcap_recover_timeout,
                                                    state);
    }
}

static gboolean source_worker_vfmcap_recover_timeout(gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;
    uint32_t width = 0;
    uint32_t height = 0;

    if (!state || state->ctx->shutting_down) {
        if (state)
            state->vfmcap_recover_timer = 0;
        return G_SOURCE_REMOVE;
    }

    if (source_worker_read_signal_resolution(state, &width, &height)) {
        if (width == state->vfmcap_stable_width && height == state->vfmcap_stable_height) {
            state->vfmcap_stable_count++;
        } else {
            LOG_I("vfmcap recovery saw HDMI signal %ux%u", width, height);
            state->vfmcap_stable_width = width;
            state->vfmcap_stable_height = height;
            state->vfmcap_stable_count = 1;
        }
    } else {
        state->vfmcap_stable_width = 0;
        state->vfmcap_stable_height = 0;
        state->vfmcap_stable_count = 0;
        return G_SOURCE_CONTINUE;
    }

    if (state->vfmcap_stable_count < SBS_VFMCAP_STABILITY_CHECK_COUNT)
        return G_SOURCE_CONTINUE;

    LOG_I("vfmcap recovery reopening after stable HDMI signal %ux%u",
          state->vfmcap_stable_width, state->vfmcap_stable_height);
    if (source_worker_vfmcap_open_start(state) != SBS_OK) {
        state->vfmcap_stable_count = 0;
        return G_SOURCE_CONTINUE;
    }

    state->vfmcap_recovering = false;
    state->vfmcap_recover_timer = 0;
    source_worker_vfmcap_reset_stats_window(state, g_get_monotonic_time(), 0);
    state->vfmcap_frame_timer = g_idle_add(on_vfmcap_frame_timeout, state);
    state->vfmcap_event_timer = g_timeout_add(100, on_vfmcap_event_timeout, state);
    sbs_worker_send_status(state->ctx, SBS_WORKER_STATE_RUNNING);
    LOG_I("vfmcap recovery complete");
    return G_SOURCE_REMOVE;
}

static gboolean on_source_control_msg(gint fd, GIOCondition cond, gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        LOG_W("supervisor connection lost");
        if (state)
            sbs_worker_request_shutdown(state->ctx);
        return G_SOURCE_REMOVE;
    }

    for (;;) {
        union {
            sbs_ipc_msg_header_t header;
            sbs_frame_release_msg_t release;
            sbs_shutdown_msg_t shutdown;
            uint8_t raw[128];
        } msg;
        size_t bytes_read = 0;
        int rc = sbs_ipc_recv_msg(fd, &msg, sizeof(msg), &bytes_read);
        if (rc == SBS_ERR_WOULD_BLOCK)
            return G_SOURCE_CONTINUE;
        if (rc == SBS_ERR_EOF || rc == SBS_ERR_PIPE) {
            LOG_I("supervisor connection closed");
            if (state)
                sbs_worker_request_shutdown(state->ctx);
            return G_SOURCE_REMOVE;
        }
        if (rc != SBS_OK) {
            LOG_W("source control recv failed: %d", rc);
            return G_SOURCE_CONTINUE;
        }
        if (bytes_read < sizeof(sbs_ipc_msg_header_t))
            continue;

        if (msg.header.msg_type == SBS_IPC_MSG_FRAME_RELEASE &&
            bytes_read >= sizeof(sbs_frame_release_msg_t)) {
            source_worker_vfmcap_release_sequence(state, msg.release.sequence);
        } else if (msg.header.msg_type == SBS_IPC_MSG_SHUTDOWN) {
            LOG_I("received SHUTDOWN from supervisor");
            sbs_worker_request_shutdown(state->ctx);
            return G_SOURCE_CONTINUE;
        }
    }
}

static gboolean on_vfmcap_frame_timeout(gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;

    if (state->ctx->shutting_down) {
        return G_SOURCE_REMOVE;
    }
    if (state->vfmcap_recovering || !state->vfmcap_ctx) {
        state->vfmcap_frame_timer = 0;
        return G_SOURCE_REMOVE;
    }

    uint32_t leases = source_worker_vfmcap_lease_count(state);
    source_worker_vfmcap_note_lease_depth(state, leases);
    if (leases >= SBS_VFMCAP_MAX_IN_FLIGHT) {
        state->vfmcap_no_lease_skips++;
        return G_SOURCE_CONTINUE;
    }

    vfmcap_frame_t frame;
    int64_t acquire_start_us = g_get_monotonic_time();
    state->vfmcap_acquire_attempts++;
    int rc = vfmcap_acquire_frame(state->vfmcap_ctx, &frame,
                                  SBS_VFMCAP_ACQUIRE_TIMEOUT_MS);
    int64_t acquire_end_us = g_get_monotonic_time();
    uint64_t acquire_us = (uint64_t)(acquire_end_us - acquire_start_us);
    state->vfmcap_acquire_total_us += acquire_us;
    if (acquire_us > state->vfmcap_acquire_max_us)
        state->vfmcap_acquire_max_us = acquire_us;

    if (rc == VFMCAP_ERR_TIMEOUT || rc == VFMCAP_ERR_NOSIG) {
        if (rc == VFMCAP_ERR_TIMEOUT) {
            state->vfmcap_acquire_timeouts++;
            state->vfmcap_consecutive_nosig = 0;
        } else {
            state->vfmcap_acquire_nosig++;
            state->vfmcap_consecutive_nosig++;
            if (state->vfmcap_consecutive_nosig >= SBS_VFMCAP_NOSIG_RECOVER_THRESHOLD) {
                state->vfmcap_frame_timer = 0;
                source_worker_vfmcap_begin_recovery(state, "sustained NOSIG");
                return G_SOURCE_REMOVE;
            }
        }
        return G_SOURCE_CONTINUE;
    }
    if (rc < 0 && rc != VFMCAP_RECONFIGURED) {
        state->vfmcap_acquire_errors++;
        LOG_W("vfmcap_acquire_frame failed: %d (%s)", rc, vfmcap_last_error(state->vfmcap_ctx));
        return G_SOURCE_CONTINUE;
    }
    state->vfmcap_acquire_ok++;
    state->vfmcap_consecutive_nosig = 0;
    if (rc == VFMCAP_RECONFIGURED)
        state->vfmcap_acquire_reconfigured++;
    if (state->vfmcap_last_success_us > 0) {
        uint64_t gap_us = (uint64_t)(acquire_end_us - state->vfmcap_last_success_us);
        state->vfmcap_success_intervals++;
        state->vfmcap_success_interval_total_us += gap_us;
        if (gap_us > state->vfmcap_success_interval_max_us)
            state->vfmcap_success_interval_max_us = gap_us;
    }
    state->vfmcap_last_success_us = acquire_end_us;

    if (frame.dmabuf_fd < 0) {
        state->vfmcap_no_fd_drops++;
        LOG_W("vfmcap frame has no DMA-BUF fd, dropping");
        vfmcap_release_frame(state->vfmcap_ctx, &frame);
        return G_SOURCE_CONTINUE;
    }

    uint32_t p010_fourcc = v4l2_fourcc('P', '0', '1', '0');
    bool frame_is_amly = frame.pixelformat == SBS_DRM_FORMAT_AMLY;
    bool frame_is_p010 = frame.pixelformat == p010_fourcc ||
                         state->vfmcap_output_format == VFMCAP_FMT_P010;
    uint32_t drm_format = frame_is_amly ? SBS_DRM_FORMAT_AMLY :
        (frame_is_p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12);
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t y_size;
    uint32_t uv_size;
    uint32_t uv_offset;

    if (frame_is_amly) {
        uint32_t packed_stride = ((frame.width + 1u) / 2u) * 5u;
        y_stride = frame.bytesperline ? frame.bytesperline : packed_stride;
        uv_stride = 0;
        y_size = frame.size ? frame.size : y_stride * frame.height;
        uv_size = 0;
        uv_offset = 0;
    } else {
        uint32_t bytes_per_sample = frame_is_p010 ? 2u : 1u;
        uint32_t compact_stride = frame.width * bytes_per_sample;
        y_stride = frame.bytesperline ? frame.bytesperline : compact_stride;
        uv_stride = y_stride;
        y_size = y_stride * frame.height;
        uv_size = uv_stride * (frame.height / 2);

        if (y_stride < compact_stride) {
            if (state->frame_counter == 0) {
                LOG_W("vfmcap reported impossible stride %u for %ux%u %s; using compact stride %u",
                      frame.bytesperline, frame.width, frame.height,
                      vfmcap_output_fmt_name(state->vfmcap_output_format), compact_stride);
            }
            y_stride = compact_stride;
            uv_stride = compact_stride;
            y_size = y_stride * frame.height;
            uv_size = uv_stride * (frame.height / 2);
        }

        /* Some libvfmcap builds report the source stride for converted linear
         * NV12/P010 output. If that layout is larger than the output frame, use
         * the compact linear stride that matches the returned plane fds. */
        if (frame.size > 0 && (uint64_t)y_size + uv_size > frame.size) {
            if (state->frame_counter == 0) {
                LOG_W("vfmcap reported stride %u exceeds output size %u; using compact stride %u",
                      frame.bytesperline, frame.size, compact_stride);
            }
            y_stride = compact_stride;
            uv_stride = compact_stride;
            y_size = y_stride * frame.height;
            uv_size = uv_stride * (frame.height / 2);
        }
        uv_offset = frame.dmabuf_fd2 >= 0 ? 0 : y_size;
    }

    if (frame.dmabuf_fd2 >= 0 && frame.dmabuf_fd2 != frame.dmabuf_fd) {
        LOG_W("vfmcap returned split-plane DMA-BUF; direct zero-copy path requires contiguous P010/NV12");
        state->vfmcap_split_plane_drops++;
        state->frames_dropped++;
        vfmcap_release_frame(state->vfmcap_ctx, &frame);
        return G_SOURCE_CONTINUE;
    }

    int fd = frame.dmabuf_fd;
    int fd2 = frame.dmabuf_fd2;
    int lease_idx = source_worker_vfmcap_store_lease(state, state->frame_counter, &frame);
    if (lease_idx < 0) {
        LOG_W("vfmcap lease ring full, dropping frame");
        state->vfmcap_lease_full_drops++;
        state->frames_dropped++;
        vfmcap_release_frame(state->vfmcap_ctx, &frame);
        return G_SOURCE_CONTINUE;
    }

    /* Build and send IPC frame message */
    sbs_video_frame_msg_t msg;
    sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_VIDEO_FRAME);
    msg.pts_ns      = frame.timestamp_us * 1000ULL;
    msg.sequence    = state->frame_counter;
    msg.width       = frame.width;
    msg.height      = frame.height;
    msg.drm_format  = drm_format;
    msg.drm_modifier = 0;
    msg.n_planes    = 1;
    msg.plane_offset[0] = 0;
    msg.plane_offset[1] = uv_offset;
    msg.plane_stride[0] = y_stride;
    msg.plane_stride[1] = uv_stride;
    msg.buffer_type = SBS_FRAME_BUFFER_DMABUF;
    msg.dmabuf_fd2 = fd2;
    msg.flags |= SBS_FRAME_FLAG_NEEDS_RELEASE;
    if (frame.signal_type || frame.bitdepth > 8) {
        msg.flags |= SBS_FRAME_FLAG_HDR;
    }

    if (state->frame_counter == 0 || state->frame_counter % 60 == 0) {
        LOG_I("vfmcap frame %lu %ux%u fmt=%s stride=%u off1=%u fd=%d fd2=%d",
              (unsigned long)state->frame_counter,
              msg.width, msg.height,
               vfmcap_output_fmt_name(state->vfmcap_output_format),
               msg.plane_stride[0], msg.plane_offset[1],
               fd, fd2);
    }

    int send_rc = sbs_ipc_send_frame2(state->ctx->sock_fd, &msg, fd, fd2);
    if (send_rc == SBS_ERR_WOULD_BLOCK) {
        state->vfmcap_send_would_block++;
        state->frames_dropped++;
        source_worker_vfmcap_release_lease_index(state, (uint32_t)lease_idx);
    } else if (send_rc == SBS_ERR_PIPE) {
        state->vfmcap_send_errors++;
        LOG_W("vfmcap send_frame: broken pipe — supervisor disconnected, stopping");
        source_worker_vfmcap_release_lease_index(state, (uint32_t)lease_idx);
        return G_SOURCE_REMOVE;
    } else if (send_rc != SBS_OK) {
        state->vfmcap_send_errors++;
        LOG_W("vfmcap send_frame failed: %d", send_rc);
        state->frames_dropped++;
        source_worker_vfmcap_release_lease_index(state, (uint32_t)lease_idx);
    } else {
        state->frame_counter++;
    }

    if (rc == VFMCAP_RECONFIGURED) {
        LOG_I("vfmcap dynamic reconfiguration detected");
    }

    return G_SOURCE_CONTINUE;
}

static gboolean on_vfmcap_event_timeout(gpointer user_data)
{
    source_state_t *state = (source_state_t *)user_data;

    if (state->ctx->shutting_down) {
        return G_SOURCE_REMOVE;
    }
    if (state->vfmcap_recovering || !state->vfmcap_ctx) {
        state->vfmcap_event_timer = 0;
        return G_SOURCE_REMOVE;
    }

    int ev = vfmcap_poll_event(state->vfmcap_ctx, 0);
    if (ev == VFMCAP_EVENT_TIMEOUT || ev == 0) {
        return G_SOURCE_CONTINUE;
    }
    if (ev < 0) {
        LOG_W("vfmcap_poll_event error: %d (%s)", ev, vfmcap_last_error(state->vfmcap_ctx));
        return G_SOURCE_CONTINUE;
    }

    if (ev == VFMCAP_EVENT_SOURCE_CHANGE) {
        LOG_I("vfmcap source change event; restarting vfmcap after stable signal");
        vfmcap_signal_info_t info;
        if (vfmcap_get_signal_info(state->vfmcap_ctx, &info) == VFMCAP_OK) {
            send_vfmcap_signal_info(state, "signal-change", &info);
        }
        state->vfmcap_event_timer = 0;
        source_worker_vfmcap_begin_recovery(state, "source-change event");
        return G_SOURCE_REMOVE;
    } else if (ev == VFMCAP_EVENT_NOSIG) {
        LOG_W("vfmcap no signal event; restarting vfmcap after stable signal");
        state->vfmcap_event_timer = 0;
        source_worker_vfmcap_begin_recovery(state, "NOSIG event");
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/* ── Source Worker Entry Point ────────────────────────────────── */

int source_worker_run(sbs_worker_ctx_t *ctx)
{
    source_state_t state = {
        .ctx             = ctx,
        .pipeline        = NULL,
        .appsink         = NULL,
        .frame_counter   = 0,
        .frames_dropped  = 0,
        .heartbeat_timer = 0,
        .text_frame_timer = 0,
        .dmabuf_available = false,
        .cpu_export_ring_idx = 0,
        .vfmcap_ctx      = NULL,
        .vfmcap_output_format = VFMCAP_FMT_RAW,
        .vfmcap_frame_timer = 0,
        .vfmcap_event_timer = 0,
        .vfmcap_stats_timer = 0,
        .vfmcap_recover_timer = 0,
        .socket_watch_id = 0,
        .vfmcap_recovering = false,
        .vfmcap_consecutive_nosig = 0,
        .vfmcap_stable_count = 0,
        .vfmcap_stable_width = 0,
        .vfmcap_stable_height = 0,
    };

    for (uint32_t i = 0; i < SBS_CPU_EXPORT_RING_SIZE; i++) {
        state.cpu_export_ring[i].backing.fd = -1;
        state.cpu_export_ring[i].mapped = NULL;
        state.cpu_export_ring[i].size = 0;
    }

    /* Open DMA-BUF allocator for exporting frames as real DMA-BUFs.
     * GE2D hardware compositor requires DMA-BUFs, not memfd. */
    if (sbs_dmabuf_alloc_open(&state.dmabuf_alloc) == SBS_OK &&
        state.dmabuf_alloc.available) {
        state.dmabuf_available = true;
        LOG_I("DMA-BUF allocator ready (heaps: codecmm=%d gfx=%d cma=%d system=%d)",
              state.dmabuf_alloc.codecmm_fd >= 0,
              state.dmabuf_alloc.gfx_fd >= 0,
              state.dmabuf_alloc.linux_cma_fd >= 0,
              state.dmabuf_alloc.system_fd >= 0);
    } else {
        LOG_W("DMA-BUF allocator not available, falling back to memfd (GE2D will not work)");
    }

    const char *source_type = ctx->config.source.source_type;
    if (!source_type) source_type = "videotestsrc";

    /* ── VFMcap direct path ───────────────────────────────────── */
    if (strcmp(source_type, "vfmcap") == 0) {
        LOG_I("initializing vfmcap direct capture: %ux%u@%u/%u",
              ctx->config.width, ctx->config.height,
              ctx->config.framerate_num, ctx->config.framerate_den);
        vfmcap_config_t vcfg = {0};
        vcfg.output_format = vfmcap_output_fmt_from_string(ctx->config.source.output_format);
        state.vfmcap_output_format = vcfg.output_format;
        /* Keep vfmcap at the HDMI signal size/cadence; the compositor owns
         * scaling to the canvas and fixed-clock frame repetition. */
        vcfg.target_width  = 0;
        vcfg.target_height = 0;
        vcfg.target_fps    = 0.0f;
        vcfg.color_mode = VFMCAP_COLOR_PASSTHROUGH;
        state.vfmcap_config = vcfg;

        const char *color_mode_str = ctx->config.source.capture_mode;
        if (color_mode_str && strcmp(color_mode_str, "passthrough") != 0) {
            LOG_W("vfmcap color_mode '%s' ignored; HDR/SDR conversion is now a compositor filter",
                  color_mode_str);
        }

        if (source_worker_vfmcap_open_start(&state) != SBS_OK)
            return SBS_ERR_IO;

        sbs_worker_send_status(ctx, SBS_WORKER_STATE_RUNNING);

        uint32_t hb_ms = ctx->config.heartbeat_interval_ms;
        if (hb_ms == 0) hb_ms = 5000;
        state.heartbeat_timer = g_timeout_add(hb_ms, on_heartbeat, &state);
        source_worker_vfmcap_reset_stats_window(&state, g_get_monotonic_time(), 0);
        state.vfmcap_frame_timer = g_idle_add(on_vfmcap_frame_timeout, &state);
        state.vfmcap_event_timer = g_timeout_add(100, on_vfmcap_event_timeout, &state);
        state.vfmcap_stats_timer = g_timeout_add(1000, on_vfmcap_stats_timeout, &state);
        state.socket_watch_id = g_unix_fd_add(ctx->sock_fd,
                                              G_IO_IN | G_IO_HUP | G_IO_ERR,
                                              on_source_control_msg, &state);

        sbs_worker_run(ctx);

        /* Cleanup vfmcap */
        if (state.vfmcap_frame_timer) {
            g_source_remove(state.vfmcap_frame_timer);
            state.vfmcap_frame_timer = 0;
        }
        if (state.vfmcap_event_timer) {
            g_source_remove(state.vfmcap_event_timer);
            state.vfmcap_event_timer = 0;
        }
        if (state.vfmcap_stats_timer) {
            g_source_remove(state.vfmcap_stats_timer);
            state.vfmcap_stats_timer = 0;
        }
        if (state.vfmcap_recover_timer) {
            g_source_remove(state.vfmcap_recover_timer);
            state.vfmcap_recover_timer = 0;
        }
        if (state.heartbeat_timer) {
            g_source_remove(state.heartbeat_timer);
            state.heartbeat_timer = 0;
        }
        if (state.socket_watch_id) {
            g_source_remove(state.socket_watch_id);
            state.socket_watch_id = 0;
        }

        source_worker_vfmcap_close_current(&state);

        if (state.dmabuf_available) {
            source_worker_release_cpu_export_ring(&state);
            sbs_dmabuf_alloc_close(&state.dmabuf_alloc);
        }

        LOG_I("vfmcap source worker done: %lu frames sent, %lu dropped",
              (unsigned long)state.frame_counter,
              (unsigned long)state.frames_dropped);
        return SBS_OK;
    }

    if (strcmp(source_type, "text") == 0) {
        uint32_t fps = ctx->config.framerate_num > 0 ? ctx->config.framerate_num : 1u;
        uint32_t interval_ms = MAX(33u, 1000u / fps);

        LOG_I("initializing text source: %ux%u@%u/%u text='%s'",
              ctx->config.width, ctx->config.height,
              ctx->config.framerate_num, ctx->config.framerate_den,
              ctx->config.source.text ? ctx->config.source.text : "");

        sbs_worker_send_status(ctx, SBS_WORKER_STATE_RUNNING);

        uint32_t hb_ms = ctx->config.heartbeat_interval_ms;
        if (hb_ms == 0) hb_ms = 5000;
        state.heartbeat_timer = g_timeout_add(hb_ms, on_heartbeat, &state);
        on_text_frame_timeout(&state);
        state.text_frame_timer = g_timeout_add(interval_ms, on_text_frame_timeout, &state);
        state.socket_watch_id = g_unix_fd_add(ctx->sock_fd,
                                              G_IO_IN | G_IO_HUP | G_IO_ERR,
                                              on_source_control_msg, &state);

        sbs_worker_run(ctx);

        if (state.text_frame_timer) {
            g_source_remove(state.text_frame_timer);
            state.text_frame_timer = 0;
        }
        if (state.heartbeat_timer) {
            g_source_remove(state.heartbeat_timer);
            state.heartbeat_timer = 0;
        }
        if (state.socket_watch_id) {
            g_source_remove(state.socket_watch_id);
            state.socket_watch_id = 0;
        }
        if (state.dmabuf_available) {
            source_worker_release_cpu_export_ring(&state);
            sbs_dmabuf_alloc_close(&state.dmabuf_alloc);
        }
        LOG_I("text source worker done: %lu frames sent, %lu dropped",
              (unsigned long)state.frame_counter,
              (unsigned long)state.frames_dropped);
        return SBS_OK;
    }

    /* ── GStreamer path ───────────────────────────────────────── */
    LOG_I("building pipeline: type=%s, %ux%u@%u/%u",
          source_type,
          ctx->config.width, ctx->config.height,
          ctx->config.framerate_num, ctx->config.framerate_den);

    /* Build pipeline based on source type */
    if (strcmp(source_type, "videotestsrc") == 0) {
        state.pipeline = build_videotestsrc_pipeline(&ctx->config);
    } else if (strcmp(source_type, "image") == 0) {
        state.pipeline = build_decode_pipeline(&ctx->config, true);
    } else if (strcmp(source_type, "uridecodebin") == 0) {
        state.pipeline = build_decode_pipeline(&ctx->config, false);
    } else if (strcmp(source_type, "v4l2src") == 0) {
        state.pipeline = build_v4l2src_pipeline(&ctx->config);
    } else {
        LOG_E("unknown source type: %s", source_type);
        return SBS_ERR_INVAL;
    }

    if (!state.pipeline) {
        LOG_E("pipeline construction failed");
        return SBS_ERR_IO;
    }

    /* Get appsink reference */
    state.appsink = gst_bin_get_by_name(GST_BIN(state.pipeline), "sink");
    if (!state.appsink) {
        LOG_E("appsink not found in pipeline");
        gst_object_unref(state.pipeline);
        return SBS_ERR_IO;
    }

    /* Set appsink callbacks */
    GstAppSinkCallbacks callbacks = {
        .eos         = NULL,
        .new_preroll = NULL,
        .new_sample  = on_new_sample,
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(state.appsink), &callbacks, &state, NULL);

    /* Install bus watch */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(state.pipeline));
    gst_bus_add_watch(bus, on_bus_message, &state);
    gst_object_unref(bus);

    /* Start pipeline */
    GstStateChangeReturn ret = gst_element_set_state(state.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_E("failed to set pipeline to PLAYING");
        gst_object_unref(state.appsink);
        gst_object_unref(state.pipeline);
        return SBS_ERR_IO;
    }

    /* Send RUNNING status */
    sbs_worker_send_status(ctx, SBS_WORKER_STATE_RUNNING);

    /* Start heartbeat timer */
    uint32_t hb_ms = ctx->config.heartbeat_interval_ms;
    if (hb_ms == 0) hb_ms = 5000;
    state.heartbeat_timer = g_timeout_add(hb_ms, on_heartbeat, &state);

    LOG_I("pipeline PLAYING — starting main loop");

    /* Run main loop (blocks until shutdown) */
    sbs_worker_run(ctx);

    /* Cleanup */
    LOG_I("shutting down pipeline...");

    if (state.heartbeat_timer) {
        g_source_remove(state.heartbeat_timer);
        state.heartbeat_timer = 0;
    }

    gst_element_set_state(state.pipeline, GST_STATE_NULL);

    /* Remove bus watch */
    bus = gst_pipeline_get_bus(GST_PIPELINE(state.pipeline));
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);

    gst_object_unref(state.appsink);
    gst_object_unref(state.pipeline);

    /* Close DMA-BUF allocator */
    if (state.dmabuf_available) {
        source_worker_release_cpu_export_ring(&state);
        sbs_dmabuf_alloc_close(&state.dmabuf_alloc);
    }

    LOG_I("source worker done: %lu frames sent, %lu dropped",
          (unsigned long)state.frame_counter,
          (unsigned long)state.frames_dropped);

    return SBS_OK;
}
