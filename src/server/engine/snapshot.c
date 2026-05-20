#define SBS_LOG_COMP "snapshot"

#include "sbs/snapshot.h"
#include "sbs/log.h"
#include "sbs/types.h"

#include <glib.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif

typedef struct {
    int memfd;
    sbs_video_frame_msg_t msg;
} latest_frame_t;

struct sbs_snapshot_engine {
    GMutex lock;
    GCond cond;
    latest_frame_t latest;
    uint16_t api_port;
    bool capture_pending;
};

static void close_latest(latest_frame_t *latest)
{
    if (latest->memfd >= 0) {
        close(latest->memfd);
        latest->memfd = -1;
    }
    memset(&latest->msg, 0, sizeof(latest->msg));
}

sbs_snapshot_engine_t *sbs_snapshot_engine_new(void)
{
    sbs_snapshot_engine_t *engine = g_new0(sbs_snapshot_engine_t, 1);
    g_mutex_init(&engine->lock);
    g_cond_init(&engine->cond);
    engine->latest.memfd = -1;
    engine->api_port = 10086;
    return engine;
}

void sbs_snapshot_engine_free(sbs_snapshot_engine_t *engine)
{
    if (!engine) return;
    g_mutex_lock(&engine->lock);
    close_latest(&engine->latest);
    g_mutex_unlock(&engine->lock);
    g_mutex_clear(&engine->lock);
    g_cond_clear(&engine->cond);
    g_free(engine);
}

void sbs_snapshot_engine_set_api_port(sbs_snapshot_engine_t *engine, uint16_t port)
{
    if (engine) engine->api_port = port;
}

bool sbs_snapshot_engine_needs_frame(const sbs_snapshot_engine_t *engine)
{
    bool needs_frame;

    if (!engine) {
        return false;
    }

    g_mutex_lock((GMutex *)&engine->lock);
    needs_frame = engine->capture_pending;
    g_mutex_unlock((GMutex *)&engine->lock);
    return needs_frame;
}

void sbs_snapshot_engine_consume_frame(sbs_snapshot_engine_t *engine,
                                       const sbs_video_frame_msg_t *msg,
                                       int memfd)
{
    if (!engine || !msg || memfd < 0) {
        if (memfd >= 0) close(memfd);
        return;
    }
    g_mutex_lock(&engine->lock);
    if (!engine->capture_pending) {
        g_mutex_unlock(&engine->lock);
        close(memfd);
        return;
    }
    close_latest(&engine->latest);
    engine->latest.memfd = memfd;
    engine->latest.msg = *msg;
    engine->capture_pending = false;
    g_cond_signal(&engine->cond);
    g_mutex_unlock(&engine->lock);
}

static void nv21_to_rgb(const uint8_t *src, const sbs_video_frame_msg_t *msg, uint8_t *rgb)
{
    const uint32_t width = msg->width;
    const uint32_t height = msg->height;
    const uint32_t y_stride = msg->plane_stride[0] ? msg->plane_stride[0] : width;
    const uint32_t uv_stride = msg->plane_stride[1] ? msg->plane_stride[1] : width;
    const uint32_t uv_offset = msg->plane_offset[1] ? msg->plane_offset[1] : y_stride * height;
    const uint8_t *y_plane = src + msg->plane_offset[0];
    const uint8_t *vu_plane = src + uv_offset;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *y_row = y_plane + y * y_stride;
        const uint8_t *vu_row = vu_plane + (y / 2u) * uv_stride;
        for (uint32_t x = 0; x < width; x++) {
            int Y = y_row[x];
            int vu_index = x & ~1u;
            int V = vu_row[vu_index] - 128;
            int U = vu_row[vu_index + 1] - 128;
            int C = Y;
            int R = C + (359 * V >> 8);
            int G = C - ((88 * U + 183 * V) >> 8);
            int B = C + (454 * U >> 8);
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            rgb[(y * width + x) * 3 + 0] = (uint8_t)R;
            rgb[(y * width + x) * 3 + 1] = (uint8_t)G;
            rgb[(y * width + x) * 3 + 2] = (uint8_t)B;
        }
    }
}

static void p010_to_rgb(const uint8_t *src, const sbs_video_frame_msg_t *msg, uint8_t *rgb)
{
    const uint32_t width = msg->width;
    const uint32_t height = msg->height;
    const uint32_t y_stride = msg->plane_stride[0] ? msg->plane_stride[0] : width * 2u;
    const uint32_t uv_stride = msg->plane_stride[1] ? msg->plane_stride[1] : width * 2u;
    const uint32_t uv_offset = msg->plane_offset[1] ? msg->plane_offset[1] : y_stride * height;
    const uint8_t *y_plane = src + msg->plane_offset[0];
    const uint8_t *uv_plane = src + uv_offset;

    for (uint32_t y = 0; y < height; y++) {
        const uint16_t *y_row = (const uint16_t *)(const void *)(y_plane + y * y_stride);
        const uint16_t *uv_row = (const uint16_t *)(const void *)(uv_plane + (y / 2u) * uv_stride);
        for (uint32_t x = 0; x < width; x++) {
            int Y = (int)(y_row[x] >> 6);
            int U = (int)(uv_row[(x & ~1u)] >> 6) - 512;
            int V = (int)(uv_row[(x & ~1u) + 1u] >> 6) - 512;
            int C = Y - 64;
            int R = (((1192 * C + 1836 * V + 512) >> 10) >> 2);
            int G = (((1192 * C - 218 * U - 547 * V + 512) >> 10) >> 2);
            int B = (((1192 * C + 2163 * U + 512) >> 10) >> 2);
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            rgb[(y * width + x) * 3 + 0] = (uint8_t)R;
            rgb[(y * width + x) * 3 + 1] = (uint8_t)G;
            rgb[(y * width + x) * 3 + 2] = (uint8_t)B;
        }
    }
}

static int write_jpeg(const char *path, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *fp = fopen(path, "wb");
    if (!fp) return SBS_ERR_IO;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)&rgb[cinfo.next_scanline * width * 3];
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return SBS_OK;
}

int sbs_snapshot_engine_capture(sbs_snapshot_engine_t *engine,
                                const char *format,
                                cJSON **metadata)
{
    latest_frame_t local = { .memfd = -1 };
    gsize total_size;
    void *mapped;
    uint8_t *rgb;
    char *id;
    char *dir;
    char *file;
    int rc;

    if (!engine || !metadata) return SBS_ERR_INVAL;

    g_mutex_lock(&engine->lock);
    close_latest(&engine->latest);
    engine->capture_pending = true;
    gint64 deadline_us = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (engine->capture_pending) {
        if (!g_cond_wait_until(&engine->cond, &engine->lock, deadline_us)) {
            engine->capture_pending = false;
            g_mutex_unlock(&engine->lock);
            return SBS_ERR_NOT_FOUND;
        }
    }
    local.memfd = dup(engine->latest.memfd);
    local.msg = engine->latest.msg;
    g_mutex_unlock(&engine->lock);
    if (local.memfd < 0) return SBS_ERR_IO;

    if (local.msg.drm_format == DRM_FORMAT_P010) {
        uint32_t y_stride = local.msg.plane_stride[0] ? local.msg.plane_stride[0] : local.msg.width * 2u;
        uint32_t uv_stride = local.msg.plane_stride[1] ? local.msg.plane_stride[1] : local.msg.width * 2u;
        uint32_t uv_offset = local.msg.plane_offset[1] ? local.msg.plane_offset[1] : y_stride * local.msg.height;
        total_size = (gsize)uv_offset + (gsize)uv_stride * (local.msg.height / 2u);
    } else {
        uint32_t y_stride = local.msg.plane_stride[0] ? local.msg.plane_stride[0] : local.msg.width;
        uint32_t uv_stride = local.msg.plane_stride[1] ? local.msg.plane_stride[1] : local.msg.width;
        uint32_t uv_offset = local.msg.plane_offset[1] ? local.msg.plane_offset[1] : y_stride * local.msg.height;
        total_size = (gsize)uv_offset + (gsize)uv_stride * (local.msg.height / 2u);
    }
    mapped = mmap(NULL, total_size, PROT_READ, MAP_SHARED, local.memfd, 0);
    if (mapped == MAP_FAILED) {
        close(local.memfd);
        return SBS_ERR_IO;
    }

    rgb = g_malloc(local.msg.width * local.msg.height * 3);
    if (local.msg.drm_format == DRM_FORMAT_P010) {
        p010_to_rgb(mapped, &local.msg, rgb);
    } else {
        nv21_to_rgb(mapped, &local.msg, rgb);
    }
    munmap(mapped, total_size);
    close(local.memfd);

    id = g_uuid_string_random();
    dir = g_strdup("/tmp/sbs-snapshots");
    g_mkdir_with_parents(dir, 0755);
    file = g_strdup_printf("%s/%s.jpg", dir, id);
    rc = write_jpeg(file, rgb, local.msg.width, local.msg.height);
    g_free(rgb);
    if (rc != SBS_OK) {
        g_free(id); g_free(dir); g_free(file);
        return rc;
    }

    *metadata = cJSON_CreateObject();
    cJSON_AddStringToObject(*metadata, "id", id);
    cJSON_AddStringToObject(*metadata, "format", format && *format ? format : "jpeg");
    cJSON_AddNumberToObject(*metadata, "width", local.msg.width);
    cJSON_AddNumberToObject(*metadata, "height", local.msg.height);
    cJSON_AddStringToObject(*metadata, "path", file);
    {
        char *url = g_strdup_printf("/snapshots/%s.jpg", id);
        cJSON_AddStringToObject(*metadata, "url", url);
        g_free(url);
    }
    cJSON_AddNumberToObject(*metadata, "created_at_us", (double)g_get_monotonic_time());
    g_free(id);
    g_free(dir);
    g_free(file);
    return SBS_OK;
}
