/*
 * Minimal vendored GE2D kernel/userspace ioctl header adapted from Amlogic.
 */
#ifndef SBS_VENDOR_GE2D_H_
#define SBS_VENDOR_GE2D_H_

#include <sys/ioctl.h>

#include "ge2d_port.h"

#define GE2D_IOC_MAGIC 'G'

#define DMA_FD_ATTACHED (-2)

struct config_planes_s {
    unsigned long addr;
    unsigned int w;
    unsigned int h;
};

struct src_key_ctrl_s {
    int key_enable;
    int key_color;
    int key_mask;
    int key_mode;
};

struct config_para_s {
    int src_dst_type;
    int alu_const_color;
    unsigned int src_format;
    unsigned int dst_format;
    struct config_planes_s src_planes[4];
    struct config_planes_s dst_planes[4];
    struct src_key_ctrl_s src_key;
};

struct config_planes_ion_s {
    unsigned long addr;
    unsigned int w;
    unsigned int h;
    int shared_fd;
};

struct src_dst_para_ex_s {
    int canvas_index;
    int top;
    int left;
    int width;
    int height;
    int format;
    int mem_type;
    int color;
    unsigned char x_rev;
    unsigned char y_rev;
    unsigned char fill_color_en;
    unsigned char fill_mode;
};

struct config_para_ex_s {
    struct src_dst_para_ex_s src_para;
    struct src_dst_para_ex_s src2_para;
    struct src_dst_para_ex_s dst_para;
    struct src_key_ctrl_s src_key;
    struct src_key_ctrl_s src2_key;
    int alu_const_color;
    unsigned src1_gb_alpha;
    unsigned int src2_gb_alpha;
    unsigned op_mode;
    unsigned char bitmask_en;
    unsigned char bytemask_only;
    unsigned int bitmask;
    unsigned char dst_xy_swap;
    unsigned hf_init_phase;
    int hf_rpt_num;
    unsigned hsc_start_phase_step;
    int hsc_phase_slope;
    unsigned vf_init_phase;
    int vf_rpt_num;
    unsigned vsc_start_phase_step;
    int vsc_phase_slope;
    unsigned char src1_vsc_phase0_always_en;
    unsigned char src1_hsc_phase0_always_en;
    unsigned char src1_hsc_rpt_ctrl;
    unsigned char src1_vsc_rpt_ctrl;
    struct config_planes_s src_planes[4];
    struct config_planes_s src2_planes[4];
    struct config_planes_s dst_planes[4];
};

struct config_para_ex_ion_s {
    struct src_dst_para_ex_s src_para;
    struct src_dst_para_ex_s src2_para;
    struct src_dst_para_ex_s dst_para;
    struct src_key_ctrl_s src_key;
    struct src_key_ctrl_s src2_key;
    unsigned char src1_cmult_asel;
    unsigned char src2_cmult_asel;
    unsigned char src2_cmult_ad;
    int alu_const_color;
    unsigned char src1_gb_alpha_en;
    unsigned src1_gb_alpha;
    unsigned char src2_gb_alpha_en;
    unsigned int src2_gb_alpha;
    unsigned op_mode;
    unsigned char bitmask_en;
    unsigned char bytemask_only;
    unsigned int bitmask;
    unsigned char dst_xy_swap;
    unsigned hf_init_phase;
    int hf_rpt_num;
    unsigned hsc_start_phase_step;
    int hsc_phase_slope;
    unsigned vf_init_phase;
    int vf_rpt_num;
    unsigned vsc_start_phase_step;
    int vsc_phase_slope;
    unsigned char src1_vsc_phase0_always_en;
    unsigned char src1_hsc_phase0_always_en;
    unsigned char src1_hsc_rpt_ctrl;
    unsigned char src1_vsc_rpt_ctrl;
    struct config_planes_ion_s src_planes[4];
    struct config_planes_ion_s src2_planes[4];
    struct config_planes_ion_s dst_planes[4];
};

struct config_para_ex_memtype_s {
    int ge2d_magic;
    struct config_para_ex_ion_s _ge2d_config_ex;
    unsigned int src1_mem_alloc_type;
    unsigned int src2_mem_alloc_type;
    unsigned int dst_mem_alloc_type;
    struct ge2d_matrix_s matrix_custom;
    struct ge2d_stride_s stride_custom;
};

struct config_ge2d_para_ex_s {
    union {
        struct config_para_ex_ion_s para_config_ion;
        struct config_para_ex_memtype_s para_config_memtype;
    };
};

struct rectangle_s {
    int x;
    int y;
    int w;
    int h;
};

struct ge2d_para_s {
    unsigned int color;
    struct rectangle_s src1_rect;
    struct rectangle_s src2_rect;
    struct rectangle_s dst_rect;
    int op;
};

struct ge2d_dmabuf_req_s {
    int index;
    unsigned int len;
    unsigned int dma_dir;
};

struct ge2d_dmabuf_exp_s {
    int index;
    unsigned int flags;
    int fd;
};

struct ge2d_dmabuf_attach_s {
    int dma_fd[GE2D_MAX_PLANE];
    enum ge2d_data_type_e data_type;
};

struct ge2d_clut8_t {
    unsigned int data[256];
    unsigned int count;
};

#define GE2D_CONFIG _IOW(GE2D_IOC_MAGIC, 0x00, struct config_para_s)
#define GE2D_CONFIG_EX _IOW(GE2D_IOC_MAGIC, 0x01, struct config_para_ex_s)
#define GE2D_SRCCOLORKEY _IOW(GE2D_IOC_MAGIC, 0x02, struct config_para_s)
#define GE2D_CONFIG_EX_ION _IOW(GE2D_IOC_MAGIC, 0x03, struct config_para_ex_ion_s)
#define GE2D_REQUEST_BUFF _IOW(GE2D_IOC_MAGIC, 0x04, struct ge2d_dmabuf_req_s)
#define GE2D_EXP_BUFF _IOW(GE2D_IOC_MAGIC, 0x05, struct ge2d_dmabuf_exp_s)
#define GE2D_FREE_BUFF _IOW(GE2D_IOC_MAGIC, 0x06, int)
#define GE2D_CONFIG_EX_MEM _IOW(GE2D_IOC_MAGIC, 0x07, struct config_ge2d_para_ex_s)
#define GE2D_SYNC_DEVICE _IOW(GE2D_IOC_MAGIC, 0x08, int)
#define GE2D_SYNC_CPU _IOW(GE2D_IOC_MAGIC, 0x09, int)
#define GE2D_ATTACH_DMA_FD _IOW(GE2D_IOC_MAGIC, 0x0a, struct ge2d_dmabuf_attach_s)
#define GE2D_DETACH_DMA_FD _IOW(GE2D_IOC_MAGIC, 0x0b, enum ge2d_data_type_e)
#define GE2D_SET_CLUT _IOW(GE2D_IOC_MAGIC, 0x0c, struct ge2d_clut8_t)

/* cmd queue enqueue operations */
#define GE2D_FILLRECTANGLE_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x0d, struct ge2d_para_s)
#define GE2D_BLEND_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x0e, struct ge2d_para_s)
#define GE2D_BLEND_NOALPHA_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x0f, struct ge2d_para_s)
#define GE2D_STRETCHBLIT_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x10, struct ge2d_para_s)
#define GE2D_STRETCHBLIT_NOALPHA_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x11, struct ge2d_para_s)
#define GE2D_BLIT_NOALPHA_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x12, struct ge2d_para_s)
#define GE2D_BLIT_ENQUEUE _IOW(GE2D_IOC_MAGIC, 0x13, struct ge2d_para_s)

/* cmd queue flush/submit */
#define GE2D_POST_QUEUE _IO(GE2D_IOC_MAGIC, 0x14)
#define GE2D_POST_QUEUE_NOBLOCK _IO(GE2D_IOC_MAGIC, 0x15)

#define GE2D_FILLRECTANGLE 0x46fd
#define GE2D_STRETCHBLIT 0x46fe
#define GE2D_BLIT 0x46ff
#define GE2D_BLEND 0x4700
#define GE2D_BLIT_NOALPHA 0x4701
#define GE2D_STRETCHBLIT_NOALPHA 0x4702
#define GE2D_BLEND_NOALPHA 0x4709
#define GE2D_GET_CAP 0x470b

#define GE2D_COLOR_MAP_SHIFT 20
#define GE2D_FORMAT_FULL_RANGE (1 << 16)
#define GE2D_COLOR_MAP_RGB888 (0 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_RGBA8888 (0 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_ARGB8888 (1 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_ABGR8888 (2 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_BGRA8888 (3 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_NV21 (14 << GE2D_COLOR_MAP_SHIFT)
#define GE2D_COLOR_MAP_NV12 (15 << GE2D_COLOR_MAP_SHIFT)

#endif
