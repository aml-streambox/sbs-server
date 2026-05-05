/*
 * Minimal vendored GE2D userspace header adapted from Amlogic libge2d.
 */
#ifndef GE2D_PORT_H_
#define GE2D_PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#define ge2d_fail    -1
#define ge2d_success  0

#define GE2D_MAX_PLANE 4

enum ge2d_data_type_e {
    AML_GE2D_SRC,
    AML_GE2D_SRC2,
    AML_GE2D_DST,
    AML_GE2D_TYPE_INVALID,
};

typedef enum {
    GE2D_CANVAS_OSD0 = 0,
    GE2D_CANVAS_OSD1,
    GE2D_CANVAS_ALLOC,
    GE2D_CANVAS_TYPE_INVALID,
} ge2d_canvas_t;

enum ge2d_memtype_s {
    AML_GE2D_MEM_ION,
    AML_GE2D_MEM_DMABUF,
    AML_GE2D_MEM_INVALID,
};

typedef enum {
    LAYER_MODE_INVALID = 0,
    LAYER_MODE_NON = 1,
    LAYER_MODE_PREMULTIPLIED = 2,
    LAYER_MODE_COVERAGE = 3,
} layer_mode_t;

typedef enum {
    PIXEL_FORMAT_RGBA_8888 = 1,
    PIXEL_FORMAT_RGBX_8888 = 2,
    PIXEL_FORMAT_RGB_888 = 3,
    PIXEL_FORMAT_RGB_565 = 4,
    PIXEL_FORMAT_BGRA_8888 = 5,
    PIXEL_FORMAT_YV12 = 6,
    PIXEL_FORMAT_Y8 = 7,
    PIXEL_FORMAT_YCbCr_422_SP = 0x10,
    PIXEL_FORMAT_YCrCb_420_SP = 0x11,
    PIXEL_FORMAT_YCbCr_422_UYVY = 0x14,
    PIXEL_FORMAT_BGR_888,
    PIXEL_FORMAT_YCbCr_420_SP_NV12,
    PIXEL_FORMAT_ARGB_8888,
    PIXEL_FORMAT_ABGR_8888,
    PIXEL_FORMAT_YU12,
    PIXEL_FORMAT_ARGB_1555,
    PIXEL_FORMAT_ARGB_4444,
    PIXEL_FORMAT_RGBA_4444,
    PIXEL_FORMAT_CLUT8,
    PIXEL_FORMAT_ALPHA8,
} pixel_format_t;

typedef enum {
    GE2D_ROTATION_0,
    GE2D_ROTATION_90,
    GE2D_ROTATION_180,
    GE2D_ROTATION_270,
    GE2D_MIRROR_X,
    GE2D_MIRROR_Y,
} GE2D_ROTATION;

typedef enum {
    AML_GE2D_FILLRECTANGLE,
    AML_GE2D_BLEND,
    AML_GE2D_STRETCHBLIT,
    AML_GE2D_BLIT,
    AML_GE2D_NONE,
} GE2DOP;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} rectangle_t;

typedef struct buffer_info {
    unsigned int mem_alloc_type;
    unsigned int memtype;
    char *vaddr[GE2D_MAX_PLANE];
    unsigned long offset[GE2D_MAX_PLANE];
    unsigned int canvas_w;
    unsigned int canvas_h;
    rectangle_t rect;
    int format;
    unsigned int rotation;
    int shared_fd[GE2D_MAX_PLANE];
    unsigned char plane_alpha;
    unsigned char layer_mode;
    unsigned char fill_color_en;
    unsigned int def_color;
    int plane_number;
    unsigned char endain;
} buffer_info_t;

struct ge2d_matrix_s {
    unsigned int pre_offset0;
    unsigned int pre_offset1;
    unsigned int pre_offset2;
    unsigned int coef0;
    unsigned int coef1;
    unsigned int coef2;
    unsigned int coef3;
    unsigned int coef4;
    unsigned int coef5;
    unsigned int coef6;
    unsigned int coef7;
    unsigned int coef8;
    unsigned int offset0;
    unsigned int offset1;
    unsigned int offset2;
    unsigned char sat_in_en;
};

struct ge2d_stride_s {
    unsigned int src1_stride[GE2D_MAX_PLANE];
    unsigned int src2_stride[GE2D_MAX_PLANE];
    unsigned int dst_stride[GE2D_MAX_PLANE];
};

typedef struct aml_ge2d_info {
    int ge2d_fd;
    int ion_fd;
    unsigned int offset;
    unsigned int blend_mode;
    GE2DOP ge2d_op;
    buffer_info_t src_info[2];
    buffer_info_t dst_info;
    unsigned int color;
    unsigned int gl_alpha;
    unsigned int const_color;
    unsigned int dst_op_cnt;
    int cap_attr;
    int b_src_swap;
    struct ge2d_matrix_s matrix_custom;
    struct ge2d_stride_s stride_custom;
    unsigned int reserved;
} aml_ge2d_info_t;

int CANVAS_ALIGNED(int x);
int YV12_Y_ALIGNED(int x);
int ge2d_open(void);
int ge2d_close(int fd);
int ge2d_get_cap(int fd);
int ge2d_process(int fd, aml_ge2d_info_t *pge2dinfo, int enqueue);
int ge2d_post_queue(int fd);
int ge2d_attach_dma_fd(int fd, aml_ge2d_info_t *pge2dinfo,
                       enum ge2d_data_type_e data_type);
int ge2d_config(int fd, aml_ge2d_info_t *pge2dinfo);
int ge2d_execute(int fd, aml_ge2d_info_t *pge2dinfo, int enqueue);
void ge2d_detach_dma_fd(int fd, enum ge2d_data_type_e data_type);
void sync_dst_dmabuf_to_cpu(aml_ge2d_info_t *pge2dinfo);
void sync_src_dmabuf_to_device(aml_ge2d_info_t *pge2dinfo, int src_id);

#ifdef __cplusplus
}
#endif

#endif
