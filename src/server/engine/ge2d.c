#define SBS_LOG_COMP "ge2d"

#include "sbs/ge2d.h"
#include "sbs/log.h"

#include "ge2d/aml_ge2d.h"
#include "ge2d/ge2d.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static void bp_sync(void)
{
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void reset_shared_fds(aml_ge2d_info_t *info)
{
    int i;

    for (i = 0; i < GE2D_MAX_PLANE; i++) {
        info->src_info[0].shared_fd[i] = -1;
        info->src_info[1].shared_fd[i] = -1;
        info->dst_info.shared_fd[i] = -1;
    }
}

static void init_info(const sbs_ge2d_t *ge2d, aml_ge2d_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->ge2d_fd = ge2d->fd;
    info->cap_attr = ge2d->caps;
    info->blend_mode = LAYER_MODE_PREMULTIPLIED;
    reset_shared_fds(info);
}

static int planes_for_format(uint32_t format)
{
    switch (format) {
    case PIXEL_FORMAT_YCrCb_420_SP:
    case PIXEL_FORMAT_YCbCr_420_SP_NV12:
    case PIXEL_FORMAT_YV12:
    case PIXEL_FORMAT_YU12:
    case PIXEL_FORMAT_YCbCr_422_SP:
        return 1;
    default:
        return 1;
    }
}

static uint32_t stride_for_format(uint32_t format, uint32_t width)
{
    switch (format) {
    case PIXEL_FORMAT_RGBA_8888:
    case PIXEL_FORMAT_RGBX_8888:
    case PIXEL_FORMAT_BGRA_8888:
    case PIXEL_FORMAT_ARGB_8888:
    case PIXEL_FORMAT_ABGR_8888:
        return (uint32_t)CANVAS_ALIGNED((int)(width * 4u));
    case PIXEL_FORMAT_RGB_888:
    case PIXEL_FORMAT_BGR_888:
        return (uint32_t)CANVAS_ALIGNED((int)(width * 3u));
    case PIXEL_FORMAT_RGB_565:
    case PIXEL_FORMAT_ARGB_4444:
    case PIXEL_FORMAT_RGBA_4444:
    case PIXEL_FORMAT_ARGB_1555:
    case PIXEL_FORMAT_YCbCr_422_UYVY:
        return (uint32_t)CANVAS_ALIGNED((int)(width * 2u));
    case PIXEL_FORMAT_YCrCb_420_SP:
    case PIXEL_FORMAT_YCbCr_420_SP_NV12:
    case PIXEL_FORMAT_Y8:
    case PIXEL_FORMAT_CLUT8:
    case PIXEL_FORMAT_ALPHA8:
    default:
        return (uint32_t)CANVAS_ALIGNED((int)width);
    }
}

static size_t size_for_format(uint32_t format, uint32_t width, uint32_t height)
{
    uint32_t stride = stride_for_format(format, width);

    switch (format) {
    case PIXEL_FORMAT_YCrCb_420_SP:
    case PIXEL_FORMAT_YCbCr_420_SP_NV12:
    case PIXEL_FORMAT_YV12:
    case PIXEL_FORMAT_YU12:
        return (size_t)stride * height * 3u / 2u;
    case PIXEL_FORMAT_YCbCr_422_SP:
        return (size_t)stride * height * 2u;
    default:
        return (size_t)stride * height;
    }
}

static int allocate_ge2d_buffer(sbs_ge2d_t *ge2d,
                                sbs_ge2d_buffer_t *buffer,
                                uint32_t width,
                                uint32_t height,
                                uint32_t format)
{
    size_t size = align_up(size_for_format(format, width, height), 4096u);
    int rc;

    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
    buffer->index = -1;
    memset(&buffer->backing, 0, sizeof(buffer->backing));
    buffer->backing.fd = -1;

    rc = sbs_dmabuf_alloc_buffer(&ge2d->alloc, size, 0, &buffer->backing);
    if (rc != SBS_OK) {
        LOG_E("SBS DMA-BUF allocation failed for GE2D buffer (%ux%u fmt=%u size=%zu)",
              width, height, format, size);
        return rc;
    }

    buffer->index = -1;
    buffer->fd = buffer->backing.fd;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride_for_format(format, width);
    buffer->format = format;
    buffer->size = size;
    return SBS_OK;
}

static void free_ge2d_buffer(sbs_ge2d_t *ge2d, sbs_ge2d_buffer_t *buffer)
{
    int index;

    if (!buffer)
        return;
    if (buffer->fd >= 0)
        close(buffer->fd);
    (void)ge2d;
    (void)index;
    memset(buffer, 0, sizeof(*buffer));
    buffer->fd = -1;
    buffer->index = -1;
}

static void fill_buffer_info(buffer_info_t *dst, const sbs_ge2d_buffer_t *buffer)
{
    memset(dst, 0, sizeof(*dst));
    dst->mem_alloc_type = AML_GE2D_MEM_DMABUF;
    dst->memtype = GE2D_CANVAS_ALLOC;
    dst->canvas_w = buffer->width;
    dst->canvas_h = buffer->height;
    dst->rect.x = 0;
    dst->rect.y = 0;
    dst->rect.w = (int)buffer->width;
    dst->rect.h = (int)buffer->height;
    dst->format = (int)buffer->format;
    dst->rotation = GE2D_ROTATION_0;
    dst->shared_fd[0] = buffer->fd;
    if (buffer->fd <= 2) {
        LOG_E("BUG: GE2D buffer has dangerously low fd=%d (stdio range)", buffer->fd);
    }
    dst->plane_number = planes_for_format(buffer->format);
    dst->plane_alpha = 0xff;
    dst->layer_mode = LAYER_MODE_PREMULTIPLIED;
}

static void fill_external_src(buffer_info_t *src,
                              int fd,
                              uint32_t width,
                              uint32_t height,
                              uint32_t format,
                              const sbs_ge2d_rect_t *rect,
                              uint32_t rotation)
{
    memset(src, 0, sizeof(*src));
    src->mem_alloc_type = AML_GE2D_MEM_DMABUF;
    src->memtype = GE2D_CANVAS_ALLOC;
    src->canvas_w = width;
    src->canvas_h = height;
    src->rect.x = rect ? rect->x : 0;
    src->rect.y = rect ? rect->y : 0;
    src->rect.w = rect ? rect->w : (int)width;
    src->rect.h = rect ? rect->h : (int)height;
    src->format = (int)format;
    src->rotation = rotation;
    src->shared_fd[0] = fd;
    src->plane_number = planes_for_format(format);
    src->plane_alpha = 0xff;
    src->layer_mode = LAYER_MODE_PREMULTIPLIED;
}

static int set_vapb_clock_667(void)
{
    FILE *f;
    int ret;

    f = fopen("/sys/kernel/debug/clk/vapb_0/clk_rate", "w");
    if (!f) {
        LOG_W("cannot open vapb_0 clk_rate for writing");
        return -1;
    }
    ret = fprintf(f, "667000000");
    fclose(f);
    if (ret < 0) {
        LOG_W("failed to write vapb_0 clk_rate");
        return -1;
    }

    f = fopen("/sys/kernel/debug/clk/vapb_0/clk_rate", "r");
    if (f) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f)) {
            unsigned long rate = strtoul(buf, NULL, 10);
            if (rate > 600000000) {
                LOG_I("VAPB clock set to %lu Hz (%lu MHz)", rate, rate / 1000000);
                fclose(f);
                return 0;
            }
            LOG_W("VAPB clock only %lu Hz after reparent attempt", rate);
        }
        fclose(f);
    }
    return -1;
}

int sbs_ge2d_init(sbs_ge2d_t *ge2d)
{
    if (!ge2d)
        return SBS_ERR_INVAL;

    memset(ge2d, 0, sizeof(*ge2d));
    ge2d->fd = -1;
    ge2d->fd = ge2d_open();
    if (ge2d->fd < 0) {
        ge2d->available = false;
        ge2d->fd = -1;
        LOG_W("GE2D unavailable: ge2d_open failed");
        return SBS_OK;
    }

    ge2d->caps = ge2d_get_cap(ge2d->fd);
    set_vapb_clock_667();
    if (sbs_dmabuf_alloc_open(&ge2d->alloc) != SBS_OK) {
        ge2d_close(ge2d->fd);
        ge2d->fd = -1;
        ge2d->available = false;
        LOG_W("GE2D unavailable: DMA-BUF allocator init failed");
        return SBS_OK;
    }
    ge2d->available = true;
    LOG_I("GE2D ready fd=%d caps=0x%x", ge2d->fd, ge2d->caps);
    return SBS_OK;
}

int sbs_ge2d_pool_init(sbs_ge2d_t *ge2d, uint32_t width, uint32_t height)
{
    int rc;

    if (!sbs_ge2d_is_available(ge2d))
        return SBS_ERR_NOT_FOUND;

    sbs_ge2d_pool_free(ge2d);

    LOG_I("pool_init: allocating canvas[0] %ux%u RGBA", width, height);
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &ge2d->canvases[0], width, height,
                              PIXEL_FORMAT_RGBA_8888);
    if (rc != SBS_OK)
        return rc;
    LOG_I("pool_init: canvas[0] allocated fd=%d size=%zu", ge2d->canvases[0].fd, ge2d->canvases[0].size);

    LOG_I("pool_init: allocating output[0] %ux%u NV21", width, height);
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &ge2d->outputs[0], width, height,
                              PIXEL_FORMAT_YCrCb_420_SP);
    if (rc != SBS_OK)
        return rc;
    LOG_I("pool_init: output[0] allocated fd=%d size=%zu", ge2d->outputs[0].fd, ge2d->outputs[0].size);

    LOG_I("pool_init: allocating temp[0] %ux%u RGBA", width, height);
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &ge2d->temps[0], width, height,
                              PIXEL_FORMAT_RGBA_8888);
    if (rc != SBS_OK)
        return rc;
    LOG_I("pool_init: temp[0] allocated fd=%d size=%zu", ge2d->temps[0].fd, ge2d->temps[0].size);

    ge2d->pool.canvas_count = 1;
    ge2d->pool.output_count = 1;
    ge2d->pool.temp_count = 1;

    rc = allocate_ge2d_buffer(ge2d, &ge2d->canvases[1], width, height,
                              PIXEL_FORMAT_RGBA_8888);
    if (rc == SBS_OK) {
        ge2d->pool.canvas_count = 2;
    } else {
        LOG_W("GE2D second canvas allocation failed, using single canvas");
    }

    rc = allocate_ge2d_buffer(ge2d, &ge2d->outputs[1], width, height,
                              PIXEL_FORMAT_YCrCb_420_SP);
    if (rc == SBS_OK) {
        ge2d->pool.output_count = 2;
    } else {
        LOG_W("GE2D second output allocation failed, using single output");
    }

    ge2d->pool.width = width;
    ge2d->pool.height = height;
    ge2d->pool.current_canvas = 0;
    ge2d->pool.current_output = 0;
    return SBS_OK;
}

int sbs_ge2d_pool_resize(sbs_ge2d_t *ge2d, uint32_t width, uint32_t height)
{
    return sbs_ge2d_pool_init(ge2d, width, height);
}

void sbs_ge2d_pool_free(sbs_ge2d_t *ge2d)
{
    if (!ge2d)
        return;
    free_ge2d_buffer(ge2d, &ge2d->canvases[0]);
    free_ge2d_buffer(ge2d, &ge2d->canvases[1]);
    free_ge2d_buffer(ge2d, &ge2d->outputs[0]);
    free_ge2d_buffer(ge2d, &ge2d->outputs[1]);
    free_ge2d_buffer(ge2d, &ge2d->temps[0]);
    memset(&ge2d->pool, 0, sizeof(ge2d->pool));
}

void sbs_ge2d_shutdown(sbs_ge2d_t *ge2d)
{
    if (!ge2d)
        return;
    sbs_ge2d_pool_free(ge2d);
    if (ge2d->fd >= 0)
        ge2d_close(ge2d->fd);
    sbs_dmabuf_alloc_close(&ge2d->alloc);
    ge2d->fd = -1;
    ge2d->caps = 0;
    ge2d->available = false;
    memset(&ge2d->pool, 0, sizeof(ge2d->pool));
}

bool sbs_ge2d_is_available(const sbs_ge2d_t *ge2d)
{
    return ge2d && ge2d->available;
}

int sbs_ge2d_get_caps(const sbs_ge2d_t *ge2d)
{
    return ge2d ? ge2d->caps : 0;
}

const sbs_ge2d_buffer_t *sbs_ge2d_current_canvas(const sbs_ge2d_t *ge2d)
{
    if (!ge2d || ge2d->pool.canvas_count == 0)
        return NULL;
    return &ge2d->canvases[ge2d->pool.current_canvas % ge2d->pool.canvas_count];
}

const sbs_ge2d_buffer_t *sbs_ge2d_current_output(const sbs_ge2d_t *ge2d)
{
    if (!ge2d || ge2d->pool.output_count == 0)
        return NULL;
    return &ge2d->outputs[ge2d->pool.current_output % ge2d->pool.output_count];
}

void sbs_ge2d_advance_buffers(sbs_ge2d_t *ge2d)
{
    if (!ge2d)
        return;
    if (ge2d->pool.canvas_count > 0)
        ge2d->pool.current_canvas = (ge2d->pool.current_canvas + 1u) % ge2d->pool.canvas_count;
    if (ge2d->pool.output_count > 0)
        ge2d->pool.current_output = (ge2d->pool.current_output + 1u) % ge2d->pool.output_count;
}

int sbs_ge2d_fillrect(sbs_ge2d_t *ge2d,
                      const sbs_ge2d_buffer_t *dst,
                      const sbs_ge2d_rect_t *rect,
                      uint32_t color)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d) || !dst)
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    info.ge2d_op = AML_GE2D_FILLRECTANGLE;
    info.color = color;
    info.src_info[0].canvas_w = dst->width;
    info.src_info[0].canvas_h = dst->height;
    info.src_info[0].format = (int)dst->format;
    info.src_info[0].plane_number = planes_for_format(dst->format);
    info.src_info[0].memtype = GE2D_CANVAS_TYPE_INVALID;
    fill_buffer_info(&info.dst_info, dst);
    if (rect) {
        info.dst_info.rect.x = rect->x;
        info.dst_info.rect.y = rect->y;
        info.dst_info.rect.w = rect->w;
        info.dst_info.rect.h = rect->h;
    }

    int64_t t0 = now_us();
    bp_sync();
    rc = aml_ge2d_config(&info);
    int64_t t1 = now_us();
    if (rc != ge2d_success) {
        LOG_E("fillrect config FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    rc = aml_ge2d_execute(&info);
    int64_t t2 = now_us();
    if (rc != ge2d_success) {
        LOG_E("fillrect execute FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    LOG_D("fillrect %ux%u config=%ldus exec=%ldus total=%ldus",
          dst->width, dst->height, (long)(t1-t0), (long)(t2-t1), (long)(t2-t0));
    return SBS_OK;
}

int sbs_ge2d_stretchblit(sbs_ge2d_t *ge2d,
                         int src_fd,
                         uint32_t src_width,
                         uint32_t src_height,
                         uint32_t src_format,
                         const sbs_ge2d_rect_t *src_rect,
                         const sbs_ge2d_buffer_t *dst,
                         const sbs_ge2d_rect_t *dst_rect,
                         uint32_t rotation,
                         bool attach_src)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d) || src_fd < 0 || !dst)
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    info.ge2d_op = AML_GE2D_STRETCHBLIT;
    fill_external_src(&info.src_info[0], src_fd, src_width, src_height, src_format, src_rect,
                      GE2D_ROTATION_0);
    fill_buffer_info(&info.dst_info, dst);
    info.dst_info.rotation = rotation;
    if (dst_rect) {
        info.dst_info.rect.x = dst_rect->x;
        info.dst_info.rect.y = dst_rect->y;
        info.dst_info.rect.w = dst_rect->w;
        info.dst_info.rect.h = dst_rect->h;
    }

    int64_t t0 = now_us();
    bp_sync();
    rc = aml_ge2d_config(&info);
    int64_t t1 = now_us();
    if (rc != ge2d_success) {
        LOG_E("stretchblit config FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    rc = aml_ge2d_execute(&info);
    int64_t t2 = now_us();
    if (rc != ge2d_success) {
        LOG_E("stretchblit execute FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    LOG_D("stretchblit %ux%u->%ux%u cfg=%ldus exec=%ldus total=%ldus",
          src_width, src_height, dst->width, dst->height,
          (long)(t1-t0), (long)(t2-t1), (long)(t2-t0));
    return SBS_OK;
}

int sbs_ge2d_blend(sbs_ge2d_t *ge2d,
                   int src_fd,
                   uint32_t src_width,
                   uint32_t src_height,
                   uint32_t src_format,
                   const sbs_ge2d_rect_t *src_rect,
                   const sbs_ge2d_buffer_t *src2,
                   const sbs_ge2d_rect_t *src2_rect,
                   const sbs_ge2d_buffer_t *dst,
                   const sbs_ge2d_rect_t *dst_rect,
                   uint8_t plane_alpha,
                   bool attach_src)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d) || src_fd < 0 || !src2 || !dst)
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    info.ge2d_op = AML_GE2D_BLEND;
    fill_external_src(&info.src_info[0], src_fd, src_width, src_height, src_format, src_rect,
                      GE2D_ROTATION_0);
    fill_buffer_info(&info.src_info[1], src2);
    fill_buffer_info(&info.dst_info, dst);
    info.src_info[0].plane_alpha = plane_alpha;
    info.src_info[0].layer_mode = LAYER_MODE_PREMULTIPLIED;
    info.src_info[1].layer_mode = LAYER_MODE_PREMULTIPLIED;
    if (src2_rect) {
        info.src_info[1].rect.x = src2_rect->x;
        info.src_info[1].rect.y = src2_rect->y;
        info.src_info[1].rect.w = src2_rect->w;
        info.src_info[1].rect.h = src2_rect->h;
    }
    if (dst_rect) {
        info.dst_info.rect.x = dst_rect->x;
        info.dst_info.rect.y = dst_rect->y;
        info.dst_info.rect.w = dst_rect->w;
        info.dst_info.rect.h = dst_rect->h;
    }

    int64_t t0 = now_us();
    bp_sync();
    rc = aml_ge2d_config(&info);
    int64_t t1 = now_us();
    if (rc != ge2d_success) {
        LOG_E("blend config FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    rc = aml_ge2d_execute(&info);
    int64_t t2 = now_us();
    if (rc != ge2d_success) {
        LOG_E("blend execute FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    bp_sync();
    LOG_D("blend %ux%u cfg=%ldus exec=%ldus total=%ldus",
           src_width, src_height,
           (long)(t1-t0), (long)(t2-t1), (long)(t2-t0));
    return SBS_OK;
}

int sbs_ge2d_fillrect_enqueue(sbs_ge2d_t *ge2d,
                              const sbs_ge2d_buffer_t *dst,
                              const sbs_ge2d_rect_t *rect,
                              uint32_t color)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d) || !dst)
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    info.ge2d_op = AML_GE2D_FILLRECTANGLE;
    info.color = color;
    info.src_info[0].canvas_w = dst->width;
    info.src_info[0].canvas_h = dst->height;
    info.src_info[0].format = (int)dst->format;
    info.src_info[0].plane_number = planes_for_format(dst->format);
    info.src_info[0].memtype = GE2D_CANVAS_TYPE_INVALID;
    fill_buffer_info(&info.dst_info, dst);
    if (rect) {
        info.dst_info.rect.x = rect->x;
        info.dst_info.rect.y = rect->y;
        info.dst_info.rect.w = rect->w;
        info.dst_info.rect.h = rect->h;
    }

    rc = aml_ge2d_process_enqueue(&info);
    if (rc != ge2d_success) {
        LOG_E("fillrect enqueue FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    return SBS_OK;
}

int sbs_ge2d_stretchblit_enqueue(sbs_ge2d_t *ge2d,
                                 int src_fd,
                                 uint32_t src_width,
                                 uint32_t src_height,
                                 uint32_t src_format,
                                 const sbs_ge2d_rect_t *src_rect,
                                 const sbs_ge2d_buffer_t *dst,
                                 const sbs_ge2d_rect_t *dst_rect,
                                 uint32_t rotation)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d) || src_fd < 0 || !dst)
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    info.ge2d_op = AML_GE2D_STRETCHBLIT;
    fill_external_src(&info.src_info[0], src_fd, src_width, src_height, src_format, src_rect,
                      GE2D_ROTATION_0);
    fill_buffer_info(&info.dst_info, dst);
    info.dst_info.rotation = rotation;
    if (dst_rect) {
        info.dst_info.rect.x = dst_rect->x;
        info.dst_info.rect.y = dst_rect->y;
        info.dst_info.rect.w = dst_rect->w;
        info.dst_info.rect.h = dst_rect->h;
    }

    rc = aml_ge2d_process_enqueue(&info);
    if (rc != ge2d_success) {
        LOG_E("stretchblit enqueue FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    return SBS_OK;
}

int sbs_ge2d_flush(sbs_ge2d_t *ge2d)
{
    aml_ge2d_info_t info;
    int rc;

    if (!sbs_ge2d_is_available(ge2d))
        return SBS_ERR_INVAL;

    init_info(ge2d, &info);
    rc = aml_ge2d_post_queue(&info);
    if (rc != ge2d_success) {
        LOG_E("ge2d post_queue FAILED rc=%d", rc);
        return SBS_ERR_IO;
    }
    return SBS_OK;
}

int sbs_ge2d_sync_device(void *ge2d_info, int src_id)
{
    aml_ge2d_info_t *info = ge2d_info;
    if (!info)
        return SBS_ERR_INVAL;
    aml_ge2d_sync_for_device(info, src_id);
    return SBS_OK;
}

int sbs_ge2d_sync_cpu(void *ge2d_info)
{
    aml_ge2d_info_t *info = ge2d_info;
    if (!info)
        return SBS_ERR_INVAL;
    aml_ge2d_sync_for_cpu(info);
    return SBS_OK;
}

int sbs_ge2d_test_fillrect(sbs_ge2d_t *ge2d)
{
    sbs_ge2d_buffer_t buf;
    sbs_ge2d_rect_t rect;
    int rc;

    if (!sbs_ge2d_is_available(ge2d))
        return SBS_ERR_INVAL;

    LOG_I("[TEST] allocating 256x256 RGBA test buffer");
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &buf, 256, 256, PIXEL_FORMAT_RGBA_8888);
    if (rc != SBS_OK) {
        LOG_E("[TEST] buffer alloc failed");
        return rc;
    }
    LOG_I("[TEST] buffer allocated fd=%d size=%zu", buf.fd, buf.size);
    bp_sync();

    rect.x = 0; rect.y = 0; rect.w = 256; rect.h = 256;
    LOG_I("[TEST] calling fillrect 256x256...");
    bp_sync();
    rc = sbs_ge2d_fillrect(ge2d, &buf, &rect, 0xFF0000FF);
    bp_sync();
    if (rc == SBS_OK)
        LOG_I("[TEST] fillrect 256x256 SUCCESS");
    else
        LOG_E("[TEST] fillrect 256x256 FAILED rc=%d", rc);
    free_ge2d_buffer(ge2d, &buf);

    if (rc != SBS_OK)
        return rc;

    sbs_ge2d_buffer_t big_buf;
    LOG_I("[TEST] allocating 1920x1080 RGBA test buffer");
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &big_buf, 1920, 1080, PIXEL_FORMAT_RGBA_8888);
    if (rc != SBS_OK) {
        LOG_E("[TEST] big buffer alloc failed");
        return rc;
    }
    LOG_I("[TEST] big buffer allocated fd=%d size=%zu", big_buf.fd, big_buf.size);
    bp_sync();

    rect.x = 0; rect.y = 0; rect.w = 1920; rect.h = 1080;
    LOG_I("[TEST] calling fillrect 1920x1080...");
    bp_sync();
    rc = sbs_ge2d_fillrect(ge2d, &big_buf, &rect, 0xFF00FF00);
    bp_sync();
    if (rc == SBS_OK)
        LOG_I("[TEST] fillrect 1920x1080 SUCCESS");
    else
        LOG_E("[TEST] fillrect 1920x1080 FAILED rc=%d", rc);
    free_ge2d_buffer(ge2d, &big_buf);

    if (rc != SBS_OK)
        return rc;

    sbs_ge2d_buffer_t nv21_buf;
    LOG_I("[TEST] allocating 1920x1080 NV21 test buffer");
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &nv21_buf, 1920, 1080, PIXEL_FORMAT_YCrCb_420_SP);
    if (rc != SBS_OK) {
        LOG_E("[TEST] NV21 buffer alloc failed");
        return rc;
    }
    LOG_I("[TEST] NV21 buffer allocated fd=%d size=%zu", nv21_buf.fd, nv21_buf.size);
    bp_sync();

    rect.x = 0; rect.y = 0; rect.w = 1920; rect.h = 1080;
    LOG_I("[TEST] calling fillrect 1920x1080 NV21...");
    bp_sync();
    rc = sbs_ge2d_fillrect(ge2d, &nv21_buf, &rect, 0xFF000000);
    bp_sync();
    if (rc == SBS_OK)
        LOG_I("[TEST] fillrect 1920x1080 NV21 SUCCESS");
    else
        LOG_E("[TEST] fillrect 1920x1080 NV21 FAILED rc=%d", rc);
    free_ge2d_buffer(ge2d, &nv21_buf);

    if (rc != SBS_OK)
        return rc;

    sbs_ge2d_buffer_t src_buf, dst_buf;
    LOG_I("[TEST] allocating src 1920x1080 NV21 for stretchblit test");
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &src_buf, 1920, 1080, PIXEL_FORMAT_YCrCb_420_SP);
    if (rc != SBS_OK) {
        LOG_E("[TEST] stretchblit src alloc failed");
        return rc;
    }
    LOG_I("[TEST] src allocated fd=%d size=%zu", src_buf.fd, src_buf.size);
    bp_sync();

    LOG_I("[TEST] allocating dst 1920x1080 RGBA for stretchblit test");
    bp_sync();
    rc = allocate_ge2d_buffer(ge2d, &dst_buf, 1920, 1080, PIXEL_FORMAT_RGBA_8888);
    if (rc != SBS_OK) {
        LOG_E("[TEST] stretchblit dst alloc failed");
        free_ge2d_buffer(ge2d, &src_buf);
        return rc;
    }
    LOG_I("[TEST] dst allocated fd=%d size=%zu", dst_buf.fd, dst_buf.size);
    bp_sync();

    rect.x = 0; rect.y = 0; rect.w = 1920; rect.h = 1080;
    LOG_I("[TEST] calling stretchblit NV21->RGBA 1920x1080...");
    bp_sync();
    rc = sbs_ge2d_stretchblit(ge2d, src_buf.fd, 1920, 1080,
                               PIXEL_FORMAT_YCrCb_420_SP, &rect,
                               &dst_buf, &rect, GE2D_ROTATION_0, true);
    bp_sync();
    if (rc == SBS_OK)
        LOG_I("[TEST] stretchblit SUCCESS");
    else
        LOG_E("[TEST] stretchblit FAILED rc=%d", rc);
    free_ge2d_buffer(ge2d, &src_buf);
    free_ge2d_buffer(ge2d, &dst_buf);

    return rc;
}
