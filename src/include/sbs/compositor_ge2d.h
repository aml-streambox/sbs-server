#ifndef SBS_COMPOSITOR_GE2D_H
#define SBS_COMPOSITOR_GE2D_H

#include "sbs/compositor_scene.h"
#include "sbs/ge2d.h"

#include <stdint.h>

int sbs_compositor_ge2d_compose_frame(sbs_ge2d_t *ge2d,
                                      uint32_t canvas_width,
                                      uint32_t canvas_height,
                                      const sbs_comp_scene_state_t *scene,
                                      const int *source_fds,
                                      uint32_t source_fd_count,
                                      const sbs_ge2d_buffer_t **out_buffer);

#endif
