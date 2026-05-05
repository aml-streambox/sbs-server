#ifndef SBS_GE2D_H
#define SBS_GE2D_H

#include "sbs/dmabuf_alloc.h"
#include "sbs/types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct sbs_ge2d_pool {
    uint32_t width;
    uint32_t height;
    uint32_t canvas_count;
    uint32_t output_count;
    uint32_t temp_count;
    uint32_t current_canvas;
    uint32_t current_output;
} sbs_ge2d_pool_t;

typedef struct sbs_ge2d_rect {
    int x;
    int y;
    int w;
    int h;
} sbs_ge2d_rect_t;

typedef struct sbs_ge2d_buffer {
    int index;
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    size_t size;
    sbs_dmabuf_buffer_t backing;
} sbs_ge2d_buffer_t;

typedef struct sbs_ge2d {
    int fd;
    int caps;
    bool available;
    sbs_dmabuf_alloc_t alloc;
    sbs_ge2d_pool_t pool;
    sbs_ge2d_buffer_t canvases[2];
    sbs_ge2d_buffer_t outputs[2];
    sbs_ge2d_buffer_t temps[1];
} sbs_ge2d_t;

int sbs_ge2d_init(sbs_ge2d_t *ge2d);
void sbs_ge2d_shutdown(sbs_ge2d_t *ge2d);
bool sbs_ge2d_is_available(const sbs_ge2d_t *ge2d);
int sbs_ge2d_get_caps(const sbs_ge2d_t *ge2d);
int sbs_ge2d_pool_init(sbs_ge2d_t *ge2d, uint32_t width, uint32_t height);
int sbs_ge2d_pool_resize(sbs_ge2d_t *ge2d, uint32_t width, uint32_t height);
void sbs_ge2d_pool_free(sbs_ge2d_t *ge2d);
const sbs_ge2d_buffer_t *sbs_ge2d_current_canvas(const sbs_ge2d_t *ge2d);
const sbs_ge2d_buffer_t *sbs_ge2d_current_output(const sbs_ge2d_t *ge2d);
void sbs_ge2d_advance_buffers(sbs_ge2d_t *ge2d);
int sbs_ge2d_fillrect(sbs_ge2d_t *ge2d,
                      const sbs_ge2d_buffer_t *dst,
                      const sbs_ge2d_rect_t *rect,
                      uint32_t color);
int sbs_ge2d_stretchblit(sbs_ge2d_t *ge2d,
                         int src_fd,
                         uint32_t src_width,
                         uint32_t src_height,
                         uint32_t src_format,
                         const sbs_ge2d_rect_t *src_rect,
                         const sbs_ge2d_buffer_t *dst,
                         const sbs_ge2d_rect_t *dst_rect,
                         uint32_t rotation,
                         bool attach_src);
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
                   bool attach_src);
int sbs_ge2d_fillrect_enqueue(sbs_ge2d_t *ge2d,
                              const sbs_ge2d_buffer_t *dst,
                              const sbs_ge2d_rect_t *rect,
                              uint32_t color);
int sbs_ge2d_stretchblit_enqueue(sbs_ge2d_t *ge2d,
                                 int src_fd,
                                 uint32_t src_width,
                                 uint32_t src_height,
                                 uint32_t src_format,
                                 const sbs_ge2d_rect_t *src_rect,
                                 const sbs_ge2d_buffer_t *dst,
                                 const sbs_ge2d_rect_t *dst_rect,
                                 uint32_t rotation);
int sbs_ge2d_flush(sbs_ge2d_t *ge2d);
int sbs_ge2d_sync_device(void *ge2d_info, int src_id);
int sbs_ge2d_sync_cpu(void *ge2d_info);
int sbs_ge2d_test_fillrect(sbs_ge2d_t *ge2d);

#endif
