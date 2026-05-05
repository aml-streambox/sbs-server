/*
 * Minimal vendored GE2D userspace header adapted from Amlogic libge2d.
 */
#ifndef GE2D_COM_H_
#define GE2D_COM_H_

#include "ge2d_port.h"

typedef struct {
    unsigned int color;
    rectangle_t src1_rect;
    rectangle_t src2_rect;
    rectangle_t dst_rect;
    int op;
} ge2d_op_para_t;

typedef struct {
    unsigned int color_blending_mode;
    unsigned int color_blending_src_factor;
    unsigned int color_blending_dst_factor;
    unsigned int alpha_blending_mode;
    unsigned int alpha_blending_src_factor;
    unsigned int alpha_blending_dst_factor;
} ge2d_blend_op;

#endif
