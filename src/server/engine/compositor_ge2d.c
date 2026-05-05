#define SBS_LOG_COMP "comp-ge2d"

#include "sbs/compositor_ge2d.h"
#include "sbs/log.h"

#include "ge2d/aml_ge2d.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int clamp_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

static uint32_t rgba_from_scene_color(const float rgba[4])
{
    uint32_t r = (uint32_t)clamp_u8((int)lroundf(rgba[0] * 255.0f));
    uint32_t g = (uint32_t)clamp_u8((int)lroundf(rgba[1] * 255.0f));
    uint32_t b = (uint32_t)clamp_u8((int)lroundf(rgba[2] * 255.0f));
    uint32_t a = (uint32_t)clamp_u8((int)lroundf(rgba[3] * 255.0f));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t map_rotation(const sbs_comp_scene_item_t *item)
{
    int deg;

    if (!item)
        return GE2D_ROTATION_0;
    if (item->flip_horizontal)
        return GE2D_MIRROR_X;
    if (item->flip_vertical)
        return GE2D_MIRROR_Y;

    deg = ((int)lroundf(item->rotation_deg) % 360 + 360) % 360;
    switch (deg) {
    case 90:
        return GE2D_ROTATION_90;
    case 180:
        return GE2D_ROTATION_180;
    case 270:
        return GE2D_ROTATION_270;
    default:
        return GE2D_ROTATION_0;
    }
}

static bool item_is_opaque(const sbs_comp_scene_item_t *item)
{
    return item && item->opacity >= 0.99f;
}

static bool rect_covers_canvas(const sbs_ge2d_rect_t *r,
                                uint32_t canvas_w, uint32_t canvas_h)
{
    return r && r->x == 0 && r->y == 0 &&
           (uint32_t)r->w == canvas_w && (uint32_t)r->h == canvas_h;
}

static bool item_covers_full_canvas(const sbs_comp_scene_item_t *item,
                                     uint32_t canvas_w, uint32_t canvas_h)
{
    if (!item)
        return false;
    return item->render_x == 0 && item->render_y == 0 &&
           (uint32_t)item->render_width == canvas_w &&
           (uint32_t)item->render_height == canvas_h;
}

int sbs_compositor_ge2d_compose_frame(sbs_ge2d_t *ge2d,
                                      uint32_t canvas_width,
                                      uint32_t canvas_height,
                                      const sbs_comp_scene_state_t *scene,
                                      const int *source_fds,
                                      uint32_t source_fd_count,
                                      const sbs_ge2d_buffer_t **out_buffer)
{
    const sbs_ge2d_buffer_t *output;
    sbs_ge2d_rect_t full_rect;
    uint32_t i;
    int rc;
    int64_t t_frame, t_op;
    int write_idx;
    const sbs_ge2d_buffer_t *active_canvas;

    if (!ge2d || !scene || !out_buffer)
        return SBS_ERR_INVAL;
    if (!sbs_ge2d_is_available(ge2d))
        return SBS_ERR_NOT_FOUND;

    if (ge2d->pool.width != canvas_width || ge2d->pool.height != canvas_height) {
        LOG_I("ge2d compose: pool resize to %ux%u", canvas_width, canvas_height);
        rc = sbs_ge2d_pool_resize(ge2d, canvas_width, canvas_height);
        if (rc != SBS_OK)
            return rc;
    }

    output = sbs_ge2d_current_output(ge2d);
    full_rect.x = 0;
    full_rect.y = 0;
    full_rect.w = (int)canvas_width;
    full_rect.h = (int)canvas_height;

    write_idx = ge2d->pool.current_canvas % ge2d->pool.canvas_count;
    active_canvas = &ge2d->canvases[write_idx];

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t_frame = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    bool need_fillrect = true;
    for (i = 0; i < scene->active_item_count && i < source_fd_count; i++) {
        const sbs_comp_scene_item_t *item = &scene->active_items[i];
        if (!item->visible)
            continue;
        if (source_fds[i] < 0 || item->frame_width == 0 || item->frame_height == 0)
            continue;
        if (item_is_opaque(item) && item_covers_full_canvas(item, canvas_width, canvas_height)) {
            need_fillrect = false;
            break;
        }
    }

    if (need_fillrect) {
        rc = sbs_ge2d_fillrect(ge2d, active_canvas, &full_rect,
                               rgba_from_scene_color(scene->background_rgba));
        if (rc != SBS_OK)
            return rc;
    }

    bool canvas_used_blend = false;

    for (i = 0; i < scene->active_item_count && i < source_fd_count; i++) {
        const sbs_comp_scene_item_t *item = &scene->active_items[i];
        sbs_ge2d_rect_t src_rect, dst_rect;
        int src_fd;

        if (!item->visible)
            continue;
        src_fd = source_fds[i];
        if (src_fd < 0 || item->frame_width == 0 || item->frame_height == 0)
            continue;

        src_rect.x = (int)lroundf(item->crop[0] * item->frame_width);
        src_rect.y = (int)lroundf(item->crop[1] * item->frame_height);
        src_rect.w = (int)item->frame_width - src_rect.x - (int)lroundf(item->crop[2] * item->frame_width);
        src_rect.h = (int)item->frame_height - src_rect.y - (int)lroundf(item->crop[3] * item->frame_height);
        if (src_rect.w <= 0 || src_rect.h <= 0)
            continue;

        dst_rect.x = item->render_x;
        dst_rect.y = item->render_y;
        dst_rect.w = item->render_width;
        dst_rect.h = item->render_height;
        if (dst_rect.w <= 0 || dst_rect.h <= 0)
            continue;

        /* Clip destination rect to canvas bounds.
         * GE2D rejects negative coordinates and out-of-bounds rects.
         * Adjust source rect proportionally for the clipped region. */
        if (dst_rect.x < 0) {
            int clip = -dst_rect.x;
            int adj = (int)((int64_t)clip * src_rect.w / dst_rect.w);
            src_rect.x += adj;
            src_rect.w -= adj;
            dst_rect.w -= clip;
            dst_rect.x = 0;
        }
        if (dst_rect.y < 0) {
            int clip = -dst_rect.y;
            int adj = (int)((int64_t)clip * src_rect.h / dst_rect.h);
            src_rect.y += adj;
            src_rect.h -= adj;
            dst_rect.h -= clip;
            dst_rect.y = 0;
        }
        if (dst_rect.x + dst_rect.w > (int)canvas_width) {
            int clip = dst_rect.x + dst_rect.w - (int)canvas_width;
            int adj = (int)((int64_t)clip * src_rect.w / (dst_rect.w + clip));
            src_rect.w -= adj;
            dst_rect.w = (int)canvas_width - dst_rect.x;
        }
        if (dst_rect.y + dst_rect.h > (int)canvas_height) {
            int clip = dst_rect.y + dst_rect.h - (int)canvas_height;
            int adj = (int)((int64_t)clip * src_rect.h / (dst_rect.h + clip));
            src_rect.h -= adj;
            dst_rect.h = (int)canvas_height - dst_rect.y;
        }
        if (src_rect.w <= 0 || src_rect.h <= 0 || dst_rect.w <= 0 || dst_rect.h <= 0)
            continue;

        if (item_is_opaque(item)) {
            rc = sbs_ge2d_stretchblit(ge2d,
                                      src_fd,
                                      item->frame_width,
                                      item->frame_height,
                                      PIXEL_FORMAT_YCrCb_420_SP,
                                      &src_rect,
                                      active_canvas,
                                      &dst_rect,
                                      map_rotation(item),
                                      true);
            if (rc != SBS_OK) {
                LOG_W("GE2D stretchblit-to-canvas failed for item %u (%s)", i, item->source_id);
                continue;
            }
        } else {
            const sbs_ge2d_buffer_t *temp = &ge2d->temps[0];
            sbs_ge2d_rect_t temp_rect = { 0, 0, dst_rect.w, dst_rect.h };
            uint8_t plane_alpha = (uint8_t)clamp_u8((int)lroundf(item->opacity * 255.0f));

            rc = sbs_ge2d_stretchblit(ge2d,
                                      src_fd,
                                      item->frame_width,
                                      item->frame_height,
                                      PIXEL_FORMAT_YCrCb_420_SP,
                                      &src_rect,
                                      temp,
                                      &temp_rect,
                                      map_rotation(item),
                                      true);
            if (rc != SBS_OK) {
                LOG_W("GE2D stretchblit failed for item %u (%s)", i, item->source_id);
                continue;
            }

            int blend_dst_idx = (write_idx + 1) % ge2d->pool.canvas_count;
            const sbs_ge2d_buffer_t *blend_dst = &ge2d->canvases[blend_dst_idx];

            rc = sbs_ge2d_blend(ge2d,
                                temp->fd,
                                temp->width, temp->height,
                                temp->format,
                                &temp_rect,
                                active_canvas, &dst_rect,
                                blend_dst, &dst_rect,
                                plane_alpha,
                                false);
            if (rc != SBS_OK) {
                LOG_W("GE2D blend failed for item %u (%s)", i, item->source_id);
                continue;
            }

            write_idx = blend_dst_idx;
            active_canvas = blend_dst;
            canvas_used_blend = true;
        }
    }

    rc = sbs_ge2d_stretchblit(ge2d,
                              active_canvas->fd,
                              active_canvas->width,
                              active_canvas->height,
                              active_canvas->format,
                              &full_rect,
                              output,
                              &full_rect,
                              GE2D_ROTATION_0,
                              false);
    if (rc != SBS_OK)
        return rc;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    t_op = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    LOG_D("ge2d frame %lldus (fill=%s blend=%s)",
          (long)(t_op - t_frame),
          need_fillrect ? "yes" : "skip",
          canvas_used_blend ? "yes" : "no");

    *out_buffer = output;
    sbs_ge2d_advance_buffers(ge2d);
    return SBS_OK;
}
