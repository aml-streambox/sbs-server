#include "sbs/compositor_ge2d.h"

#include <string.h>

int sbs_ge2d_init(sbs_ge2d_t *ge2d)
{
    if (ge2d) {
        memset(ge2d, 0, sizeof(*ge2d));
    }
    return -1;
}

void sbs_ge2d_shutdown(sbs_ge2d_t *ge2d)
{
    (void)ge2d;
}

int sbs_ge2d_pool_init(sbs_ge2d_t *ge2d, uint32_t width, uint32_t height)
{
    (void)ge2d;
    (void)width;
    (void)height;
    return -1;
}

int sbs_ge2d_test_fillrect(sbs_ge2d_t *ge2d)
{
    (void)ge2d;
    return -1;
}

int sbs_compositor_ge2d_compose_frame(sbs_ge2d_t *ge2d,
                                      uint32_t canvas_width,
                                      uint32_t canvas_height,
                                      const sbs_comp_scene_state_t *scene,
                                      const int *source_fds,
                                      uint32_t source_fd_count,
                                      const sbs_ge2d_buffer_t **out_buffer)
{
    (void)ge2d;
    (void)canvas_width;
    (void)canvas_height;
    (void)scene;
    (void)source_fds;
    (void)source_fd_count;
    if (out_buffer) {
        *out_buffer = NULL;
    }
    return -1;
}
