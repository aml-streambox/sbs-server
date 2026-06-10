#define _GNU_SOURCE
#define SBS_LOG_COMP "direct-venc"

#include "sbs/direct_venc.h"
#include "sbs/dmabuf_alloc.h"
#include "sbs/log.h"

#include <dlfcn.h>
#include <errno.h>
#include <glib.h>
#include <linux/dma-buf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef DRM_FORMAT_NV21
#define DRM_FORMAT_NV21 0x3132564e
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif

/* ---- Types from libvpcodec (vp_multi_codec_1_0.h) ---- */

typedef long vl_codec_handle_t;

typedef enum {
    CODEC_ID_NONE,
    CODEC_ID_VP8,
    CODEC_ID_H261,
    CODEC_ID_H263,
    CODEC_ID_H264,
    CODEC_ID_H265,
} vl_codec_id_t;

typedef enum {
    IMG_FMT_NONE,
    IMG_FMT_NV12,
    IMG_FMT_NV21,
    IMG_FMT_YUV420P,
    IMG_FMT_YV12,
    IMG_FMT_RGB888,
    IMG_FMT_RGBA8888,
    IMG_FMT_P010,
} vl_img_format_t;

typedef enum {
    FRAME_TYPE_NONE,
    FRAME_TYPE_AUTO,
    FRAME_TYPE_IDR,
    FRAME_TYPE_I,
    FRAME_TYPE_P,
    FRAME_TYPE_B,
    FRAME_TYPE_DROPPABLE_P,
} vl_frame_type_t;

typedef enum {
    VMALLOC_TYPE = 0,
    CANVAS_TYPE = 1,
    PHYSICAL_TYPE = 2,
    DMA_TYPE = 3,
} vl_buffer_type_t;

typedef struct {
    int frame_type;
    int average_qp_value;
    int intra_blocks;
    int merged_blocks;
    int skipped_blocks;
} enc_frame_extra_info_t;

typedef struct {
    int encoded_data_length_in_bytes;
    bool is_key_frame;
    int timestamp_us;
    bool is_valid;
    enc_frame_extra_info_t extra;
    int err_cod;
    int input_frame_num;
} encoding_metadata_t;

typedef struct {
    int width;
    int height;
    int frame_rate;
    int bit_rate;
    int gop;
    bool prepend_spspps_to_idr_frames;
    vl_img_format_t img_format;
    int qp_mode;
    int forcePicQpEnable;
    int forcePicQpI;
    int forcePicQpP;
    int forcePicQpB;
    int enc_feature_opts;
    int intra_refresh_mode;
    int intra_refresh_arg;
    int profile;
    int level;
    uint32_t frame_rotation;
    uint32_t frame_mirroring;
    int bitstream_buf_sz;
    int multi_slice_mode;
    int multi_slice_arg;
    int cust_gop_qp_delta;
    int strict_rc_window;
    int strict_rc_skip_thresh;
    int bitstream_buf_sz_kb;
    uint8_t vui_parameters_present_flag;
    uint8_t video_full_range_flag;
    uint8_t video_signal_type_present_flag;
    uint8_t colour_description_present_flag;
    uint8_t colour_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    bool crop_enable;
    struct {
        int left;
        int top;
        int right;
        int bottom;
    } crop;
    int internal_bit_depth;
    int gop_pattern;
    int rc_mode;
    int lossless_enable;
} vl_encode_info_t;

typedef struct {
    int shared_fd[3];
    unsigned int num_planes;
} vl_dma_info_t;

typedef union {
    vl_dma_info_t dma_info;
    unsigned long in_ptr[3];
    uint32_t canvas;
} vl_buf_info_u;

typedef struct {
    vl_buffer_type_t buf_type;
    vl_buf_info_u buf_info;
    int buf_stride;
    vl_img_format_t buf_fmt;
} vl_buffer_info_t;

typedef struct {
    int qp_min;
    int qp_max;
    int qp_I_base;
    int qp_P_base;
    int qp_B_base;
    int qp_I_min;
    int qp_I_max;
    int qp_P_min;
    int qp_P_max;
    int qp_B_min;
    int qp_B_max;
} qp_param_t;

/* ---- Function pointer types ---- */

typedef vl_codec_handle_t (*vl_multi_encoder_init_fn)(vl_codec_id_t codec_id,
                                                      vl_encode_info_t encode_info,
                                                      qp_param_t *qp_tbl);
typedef encoding_metadata_t (*vl_multi_encoder_encode_fn)(vl_codec_handle_t handle,
                                                          vl_frame_type_t type,
                                                          unsigned char *out,
                                                          vl_buffer_info_t *in_buffer_info,
                                                          vl_buffer_info_t *ret_buffer_info);
typedef int (*vl_multi_encoder_destroy_fn)(vl_codec_handle_t handle);

/* ---- Pending frame tracking ---- */

typedef struct sbs_pending_frame {
    int id;
    uint64_t pts_ns;
    uint64_t dts_ns;
    uint64_t duration_ns;
    bool requested_idr;
    uint8_t *owned_input;
    size_t owned_input_size;
} sbs_pending_frame_t;

/* ---- Encoder instance ---- */

struct sbs_direct_venc {
    void *libvpcodec;
    vl_multi_encoder_init_fn init_fn;
    vl_multi_encoder_encode_fn encode_fn;
    vl_multi_encoder_destroy_fn destroy_fn;

    vl_codec_handle_t handle;
    vl_codec_id_t codec_id;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t bitrate_kbps;
    uint32_t gop_size;
    int32_t gop_pattern;
    int32_t rc_mode;
    sbs_pixel_format_t input_format;
    sbs_colorimetry_t colorimetry;
    bool hdr10;
    bool input_hdr10;
    bool bframe_enabled;

    uint8_t *outbuf;
    size_t outbuf_size;
    int next_submit_id;
    uint64_t output_counter;
    uint64_t frame_duration_ns;
    bool last_packet_dts_valid;
    uint64_t last_packet_dts_ns;
    bool input_frame_num_origin_valid;
    int input_frame_num_origin;
    int submit_id_origin;
    GQueue *pending_frames;
};

/* ---- Helpers ---- */

static vl_codec_id_t codec_id_from_string(const char *codec)
{
    return (codec && strcmp(codec, "h264") == 0) ? CODEC_ID_H264 : CODEC_ID_H265;
}

static const char *codec_id_name(vl_codec_id_t codec_id)
{
    return codec_id == CODEC_ID_H264 ? "h264" : "h265";
}

static const char *img_format_name(vl_img_format_t fmt)
{
    switch (fmt) {
    case IMG_FMT_NV12: return "NV12";
    case IMG_FMT_NV21: return "NV21";
    case IMG_FMT_YUV420P: return "YUV420P";
    case IMG_FMT_YV12: return "YV12";
    case IMG_FMT_RGB888: return "RGB888";
    case IMG_FMT_RGBA8888: return "RGBA8888";
    case IMG_FMT_P010: return "P010";
    default: return "unknown";
    }
}

static const char *buffer_type_name(vl_buffer_type_t type)
{
    switch (type) {
    case VMALLOC_TYPE: return "VMALLOC";
    case CANVAS_TYPE: return "CANVAS";
    case PHYSICAL_TYPE: return "PHYSICAL";
    case DMA_TYPE: return "DMA";
    default: return "unknown";
    }
}

static const char *drm_format_name(uint32_t drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_NV21: return "NV21";
    case DRM_FORMAT_P010: return "P010";
    case 0: return "unset";
    default: return "unknown";
    }
}

static void colorimetry_to_vui(sbs_colorimetry_t colorimetry,
                               uint8_t *primaries,
                               uint8_t *transfer,
                               uint8_t *matrix)
{
    switch (colorimetry) {
    case SBS_COLORIMETRY_BT601:
        *primaries = 6;
        *transfer = 6;
        *matrix = 6;
        break;
    case SBS_COLORIMETRY_BT2020_PQ:
        *primaries = 9;
        *transfer = 16;
        *matrix = 9;
        break;
    case SBS_COLORIMETRY_BT2100_HLG:
        *primaries = 9;
        *transfer = 18;
        *matrix = 9;
        break;
    case SBS_COLORIMETRY_BT709:
    case SBS_COLORIMETRY_SDR:
    default:
        *primaries = 1;
        *transfer = 1;
        *matrix = 1;
        break;
    }
}

static const char *h265_nal_type_name(uint8_t nal_type)
{
    switch (nal_type) {
    case 19: return "IDR_W_RADL";
    case 20: return "IDR_N_LP";
    case 32: return "VPS";
    case 33: return "SPS";
    case 34: return "PPS";
    case 35: return "AUD";
    case 39: return "PREFIX_SEI";
    case 40: return "SUFFIX_SEI";
    default:
        if (nal_type <= 9)
            return "TRAIL";
        if (nal_type >= 16 && nal_type <= 18)
            return "BLA/CRA";
        return "NAL";
    }
}

static size_t choose_outbuf_size(uint32_t width, uint32_t height)
{
    size_t base = (size_t)width * (size_t)height * 3;
    size_t min_size = 4u * 1024u * 1024u;
    return (base > min_size) ? base : min_size;
}

static int choose_bitstream_buf_sz_kb(uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    return 2048;
}

static bool sync_dmabuf_read(int fd, bool start)
{
    struct dma_buf_sync sync = {
        .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ,
    };
    if (fd < 0)
        return false;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
        return true;
    LOG_W("DMA_BUF_SYNC_%s failed on fd %d: %s",
          start ? "START" : "END", fd, g_strerror(errno));
    return false;
}

static bool sync_dmabuf_write_end(int fd)
{
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE,
    };
    if (fd < 0)
        return false;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
        return true;
    LOG_W("DMA_BUF_SYNC_END(WRITE) failed on fd %d: %s", fd, g_strerror(errno));
    return false;
}

static bool gop_pattern_has_bframes(int32_t gop_pattern)
{
    switch (gop_pattern) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 7:
        return true;
    default:
        return false;
    }
}

static uint32_t gop_pattern_delay_frames(int32_t gop_pattern)
{
    switch (gop_pattern) {
    case 1:
        return 4;
    case 2:
        return 3;
    case 3:
        return 3;
    case 6:
        return 4;
    case 7:
        return 8;
    default:
        return 0;
    }
}

static uint64_t calculate_frame_duration_ns(uint32_t fps_num, uint32_t fps_den)
{
    if (fps_num == 0)
        return 1000000000ull / 60ull;

    if (fps_den == 0)
        fps_den = 1;

    return (1000000000ull * (uint64_t)fps_den) / (uint64_t)fps_num;
}

static size_t visible_frame_size_from_msg(const sbs_video_frame_msg_t *msg,
                                          bool hdr10)
{
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t uv_offset;

    if (!msg || msg->width == 0 || msg->height == 0)
        return 0;

    if (hdr10) {
        y_stride = msg->plane_stride[0] ? msg->plane_stride[0]
                                        : msg->width * 2u;
        uv_stride = msg->plane_stride[1] ? msg->plane_stride[1]
                                         : y_stride;
        uv_offset = msg->plane_offset[1] ? msg->plane_offset[1]
                                         : y_stride * msg->height;
        return (size_t)uv_offset + (size_t)uv_stride * (msg->height / 2u);
    }

    y_stride = msg->plane_stride[0] ? msg->plane_stride[0] : msg->width;
    uv_stride = msg->plane_stride[1] ? msg->plane_stride[1] : y_stride;
    uv_offset = msg->plane_offset[1] ? msg->plane_offset[1]
                                     : y_stride * msg->height;
    return (size_t)uv_offset + (size_t)uv_stride * (msg->height / 2u);
}

static uint64_t bframe_delay_ns(const sbs_direct_venc_t *enc)
{
    uint64_t frames;

    if (!enc || !enc->bframe_enabled || enc->frame_duration_ns == 0)
        return 0;

    frames = gop_pattern_delay_frames(enc->gop_pattern);
    return frames * enc->frame_duration_ns;
}

static uint64_t calculate_packet_dts(sbs_direct_venc_t *enc,
                                     const sbs_pending_frame_t *pending,
                                     uint64_t packet_pts_ns)
{
    uint64_t delay_ns;
    uint64_t dts_ns;
    uint64_t min_next_ns;

    if (!enc || !pending)
        return UINT64_MAX;

    if (!enc->bframe_enabled || enc->frame_duration_ns == 0) {
        if (pending->dts_ns != UINT64_MAX)
            return pending->dts_ns;
        if (packet_pts_ns != UINT64_MAX)
            return packet_pts_ns;
        if (pending->pts_ns != UINT64_MAX)
            return pending->pts_ns;
        return enc->frame_duration_ns > 0
            ? (uint64_t)pending->id * enc->frame_duration_ns
            : UINT64_MAX;
    }

    if (packet_pts_ns == UINT64_MAX)
        packet_pts_ns = pending->pts_ns != UINT64_MAX
            ? pending->pts_ns
            : (uint64_t)pending->id * enc->frame_duration_ns;

    delay_ns = bframe_delay_ns(enc);
    dts_ns = packet_pts_ns >= delay_ns ? packet_pts_ns - delay_ns : packet_pts_ns;

    if (enc->last_packet_dts_valid) {
        min_next_ns = enc->last_packet_dts_ns + enc->frame_duration_ns;
        if (dts_ns < min_next_ns)
            dts_ns = min_next_ns;
    }

    enc->last_packet_dts_ns = dts_ns;
    enc->last_packet_dts_valid = true;
    return dts_ns;
}

static void pending_frame_free(gpointer data)
{
    sbs_pending_frame_t *pending = data;
    if (!pending)
        return;
    g_free(pending->owned_input);
    g_free(pending);
}

static sbs_pending_frame_t *pending_frame_take_match(sbs_direct_venc_t *enc, int input_frame_num)
{
    if (!enc || !enc->pending_frames)
        return NULL;
    if (input_frame_num >= 0) {
        int match_id = input_frame_num;
        if (enc->bframe_enabled) {
            sbs_pending_frame_t *head = g_queue_peek_head(enc->pending_frames);
            if (!enc->input_frame_num_origin_valid && head) {
                enc->input_frame_num_origin = input_frame_num;
                enc->submit_id_origin = head->id;
                enc->input_frame_num_origin_valid = true;
            }
            if (enc->input_frame_num_origin_valid)
                match_id = enc->submit_id_origin +
                           (input_frame_num - enc->input_frame_num_origin);
        }
        for (GList *it = enc->pending_frames->head; it; it = it->next) {
            sbs_pending_frame_t *pending = it->data;
            if (pending && pending->id == match_id) {
                g_queue_delete_link(enc->pending_frames, it);
                return pending;
            }
        }
    }
    return g_queue_pop_head(enc->pending_frames);
}

static void pending_frame_push(sbs_direct_venc_t *enc,
                               const sbs_video_frame_msg_t *msg,
                               bool requested_idr,
                               uint8_t *owned_input,
                               size_t owned_input_size)
{
    sbs_pending_frame_t *pending = g_new0(sbs_pending_frame_t, 1);
    pending->id = enc->next_submit_id++;
    pending->pts_ns = msg->pts_ns;
    pending->dts_ns = msg->dts_ns;
    pending->duration_ns = msg->duration_ns;
    pending->requested_idr = requested_idr;
    pending->owned_input = owned_input;
    pending->owned_input_size = owned_input_size;
    g_queue_push_tail(enc->pending_frames, pending);
}

static void pending_frame_prune(sbs_direct_venc_t *enc)
{
    uint32_t keep;
    guint length;
    guint idx = 0;

    if (!enc || !enc->pending_frames)
        return;

    keep = enc->bframe_enabled ? gop_pattern_delay_frames(enc->gop_pattern) + 4u : 2u;
    length = g_queue_get_length(enc->pending_frames);
    for (GList *it = enc->pending_frames->head; it; it = it->next, idx++) {
        sbs_pending_frame_t *pending = it->data;
        if (!pending)
            continue;
        if (idx + keep < length && pending->owned_input) {
            g_free(pending->owned_input);
            pending->owned_input = NULL;
            pending->owned_input_size = 0;
        }
    }
    while (g_queue_get_length(enc->pending_frames) > 4096u) {
        sbs_pending_frame_t *old = g_queue_pop_head(enc->pending_frames);
        pending_frame_free(old);
    }
}

/* ---- dlopen / dlsym ---- */

static int load_symbols(sbs_direct_venc_t *enc)
{
    enc->libvpcodec = dlopen("libvpcodec.so", RTLD_NOW | RTLD_LOCAL);
    if (!enc->libvpcodec) {
        LOG_E("failed to load libvpcodec.so: %s", dlerror());
        return SBS_ERR_IO;
    }

    enc->init_fn = (vl_multi_encoder_init_fn)dlsym(enc->libvpcodec, "vl_multi_encoder_init");
    enc->encode_fn = (vl_multi_encoder_encode_fn)dlsym(enc->libvpcodec, "vl_multi_encoder_encode");
    enc->destroy_fn = (vl_multi_encoder_destroy_fn)dlsym(enc->libvpcodec, "vl_multi_encoder_destroy");

    if (!enc->init_fn || !enc->encode_fn || !enc->destroy_fn) {
        LOG_E("libvpcodec missing required encoder symbols");
        return SBS_ERR_IO;
    }

    return SBS_OK;
}

static bool find_annexb_start_code(const uint8_t *data,
                                   size_t size,
                                   size_t from,
                                   size_t *prefix_pos,
                                   size_t *nal_pos)
{
    if (!data || !prefix_pos || !nal_pos || from >= size)
        return false;

    for (size_t i = from; i + 3 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *prefix_pos = i;
                *nal_pos = i + 3;
                return true;
            }
            if (i + 4 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *prefix_pos = i;
                *nal_pos = i + 4;
                return true;
            }
        }
    }
    return false;
}

static void log_h265_nal_summary(const uint8_t *data, size_t size)
{
    char summary[384];
    size_t prefix = 0;
    size_t nal = 0;
    size_t search = 0;
    int off = 0;
    uint32_t count = 0;

    if (!sbs_log_profile_enabled() || !data || size == 0)
        return;

    summary[0] = '\0';
    while (count < 12 && find_annexb_start_code(data, size, search, &prefix, &nal)) {
        size_t next_prefix = size;
        size_t next_nal = size;
        size_t nal_size;
        uint8_t nal_type;

        if (nal + 2 > size)
            break;
        if (find_annexb_start_code(data, size, nal + 2, &next_prefix, &next_nal))
            nal_size = next_prefix > nal ? next_prefix - nal : 0;
        else
            nal_size = size - nal;

        nal_type = (data[nal] >> 1) & 0x3f;
        off += snprintf(summary + off, sizeof(summary) - (size_t)off,
                        "%s%s(%u):%zu",
                        count == 0 ? "" : ",",
                        h265_nal_type_name(nal_type), nal_type, nal_size);
        if (off >= (int)sizeof(summary)) {
            summary[sizeof(summary) - 1] = '\0';
            break;
        }
        count++;
        search = next_prefix;
        if (search >= size)
            break;
    }

    if (count == 0) {
        LOG_I("VENC OUTPUT NAL: no Annex-B start codes found size=%zu first=%02x %02x %02x %02x",
              size,
              size > 0 ? data[0] : 0,
              size > 1 ? data[1] : 0,
              size > 2 ? data[2] : 0,
              size > 3 ? data[3] : 0);
    } else {
        LOG_I("VENC OUTPUT NAL: count_first=%u size=%zu %s%s",
              count, size, summary, count == 12 ? ",..." : "");
    }
}

static void log_input_contract(const sbs_direct_venc_t *enc,
                               const char *path,
                               const sbs_video_frame_msg_t *msg,
                               const vl_buffer_info_t *inbuf,
                               size_t buffer_size)
{
    size_t visible_size;

    if (!enc || !msg || !inbuf)
        return;
    if (!sbs_log_profile_enabled())
        return;
    if (enc->next_submit_id > 2 && !getenv("SBS_VENC_LOG_PARAMS"))
        return;

    visible_size = visible_frame_size_from_msg(msg, enc->input_hdr10);
    LOG_I("VENC INPUT CONTRACT[%s] submit=%d msg={%ux%u drm=%s/0x%08x n_planes=%u offsets=%u,%u strides=%u,%u buffer_type=%u size=%zu visible=%zu pts=%lu dur=%lu seq=%lu} enc={input_format=%s colorimetry=%s legacy_input_hdr10=%d legacy_hdr10=%d} inbuf={type=%s/%d fmt=%s/%d stride=%d ptr0=0x%lx ptr1=0x%lx dma_fd=%d,%d,%d dma_planes=%u}",
          path ? path : "?", enc->next_submit_id,
          msg->width, msg->height,
          drm_format_name(msg->drm_format), (unsigned)msg->drm_format,
          msg->n_planes,
          msg->plane_offset[0], msg->plane_offset[1],
          msg->plane_stride[0], msg->plane_stride[1],
          msg->buffer_type, buffer_size, visible_size,
          (unsigned long)msg->pts_ns,
          (unsigned long)msg->duration_ns,
          (unsigned long)msg->sequence,
          sbs_pixel_format_name(enc->input_format),
          sbs_colorimetry_name(enc->colorimetry),
          enc->input_hdr10 ? 1 : 0,
          enc->hdr10 ? 1 : 0,
          buffer_type_name(inbuf->buf_type), inbuf->buf_type,
          img_format_name(inbuf->buf_fmt), inbuf->buf_fmt,
          inbuf->buf_stride,
          inbuf->buf_type == VMALLOC_TYPE ? inbuf->buf_info.in_ptr[0] : 0ul,
          inbuf->buf_type == VMALLOC_TYPE ? inbuf->buf_info.in_ptr[1] : 0ul,
          inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[0] : -1,
          inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[1] : -1,
          inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[2] : -1,
          inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.num_planes : 0);
}

/* ---- Encoder init (matches gstamlvenc_init_encoder) ---- */

static int init_encoder_handle(sbs_direct_venc_t *enc)
{
    vl_encode_info_t info;
    qp_param_t qp;
    int fps = (enc->fps_num > 0 && enc->fps_den > 0)
        ? (int)(enc->fps_num / enc->fps_den)
        : 60;

    memset(&info, 0, sizeof(info));
    memset(&qp, 0, sizeof(qp));

    info.width = (int)enc->width;
    info.height = (int)enc->height;
    info.frame_rate = fps > 0 ? fps : 60;
    info.bit_rate = (int)enc->bitrate_kbps * 1000;
    info.gop = (int)enc->gop_size;
    info.prepend_spspps_to_idr_frames = true;
    info.img_format = enc->input_format == SBS_PIXEL_FORMAT_P010 ? IMG_FMT_P010 : IMG_FMT_NV21;
    info.enc_feature_opts |= 0x1;
    info.internal_bit_depth = enc->input_format == SBS_PIXEL_FORMAT_P010 ? 10 : 8;
    info.gop_pattern = enc->gop_pattern;
    info.rc_mode = enc->rc_mode;
    info.bitstream_buf_sz_kb = choose_bitstream_buf_sz_kb(enc->width, enc->height);

    info.vui_parameters_present_flag = 1;
    info.video_signal_type_present_flag = 1;
    info.video_full_range_flag = 0;
    info.colour_description_present_flag = 1;
    colorimetry_to_vui(enc->colorimetry,
                       &info.colour_primaries,
                       &info.transfer_characteristics,
                       &info.matrix_coefficients);

    qp.qp_min = 0;
    qp.qp_max = 51;
    qp.qp_I_base = 30;
    qp.qp_I_min = 0;
    qp.qp_I_max = 51;
    qp.qp_P_base = 30;
    qp.qp_P_min = 0;
    qp.qp_P_max = 51;

    LOG_I("VENC OPEN REQUEST: codec=%s(%d) size=%dx%d fps=%d bitrate=%d gop=%d input_format=%s img_format=%s(%d) colorimetry=%s enc_opts=0x%x internal_bit_depth=%d gop_pattern=%d rc_mode=%d bitstream_buf_kb=%d vui={present=%u full_range=%u signal=%u colour_desc=%u prim=%u transfer=%u matrix=%u} qp={min=%d max=%d I=%d/%d-%d P=%d/%d-%d}",
          codec_id_name(enc->codec_id), enc->codec_id,
          info.width, info.height, info.frame_rate, info.bit_rate,
          info.gop, sbs_pixel_format_name(enc->input_format),
          img_format_name(info.img_format), info.img_format,
          sbs_colorimetry_name(enc->colorimetry),
          info.enc_feature_opts, info.internal_bit_depth,
          info.gop_pattern, info.rc_mode, info.bitstream_buf_sz_kb,
          info.vui_parameters_present_flag,
          info.video_full_range_flag,
          info.video_signal_type_present_flag,
          info.colour_description_present_flag,
          info.colour_primaries,
          info.transfer_characteristics,
          info.matrix_coefficients,
          qp.qp_min, qp.qp_max,
          qp.qp_I_base, qp.qp_I_min, qp.qp_I_max,
          qp.qp_P_base, qp.qp_P_min, qp.qp_P_max);

    enc->handle = enc->init_fn(enc->codec_id, info, &qp);
    if (enc->handle == 0) {
        LOG_E("vl_multi_encoder_init failed: codec=%s %ux%u bitrate=%ukbps gop=%u gop_pattern=%d",
              codec_id_name(enc->codec_id),
              enc->width, enc->height, enc->bitrate_kbps, enc->gop_size, enc->gop_pattern);
        return SBS_ERR_IO;
    }

    LOG_I("VENC OPEN RESULT: handle=0x%lx codec=%s input_format=%s colorimetry=%s legacy_input_hdr10=%d legacy_hdr10=%d bframes=%d frame_duration=%luns outbuf=%zuKB",
          (unsigned long)enc->handle,
          codec_id_name(enc->codec_id),
          sbs_pixel_format_name(enc->input_format),
          sbs_colorimetry_name(enc->colorimetry),
          enc->input_hdr10 ? 1 : 0,
          enc->hdr10 ? 1 : 0,
          enc->bframe_enabled,
          (unsigned long)enc->frame_duration_ns,
          enc->outbuf_size / 1024u);

    return SBS_OK;
}

/* ---- Common encode path (matches gstamlvenc_encode_frame) ---- */

static int submit_common(sbs_direct_venc_t *enc,
                          const sbs_video_frame_msg_t *msg,
                          vl_buffer_info_t *inbuf,
                          int sync_fd,
                          uint8_t *owned_input,
                          size_t owned_input_size,
                          bool force_idr,
                          sbs_direct_venc_packet_t *packet)
{
    vl_buffer_info_t retbuf;
    encoding_metadata_t meta;
    bool request_idr;
    vl_frame_type_t frame_type;

    if (!enc || !msg || !inbuf)
        return SBS_ERR_INVAL;

    request_idr = force_idr || enc->next_submit_id == 0 ||
        (enc->gop_size > 0 && (enc->next_submit_id % (int)enc->gop_size) == 0);
    frame_type = request_idr ? FRAME_TYPE_IDR : FRAME_TYPE_AUTO;

    memset(packet, 0, sizeof(*packet));
    packet->dts_ns = UINT64_MAX;
    memset(&retbuf, 0, sizeof(retbuf));

    if (sbs_log_profile_enabled() && enc->next_submit_id <= 2) {
        LOG_I("submit: buf_type=%d buf_fmt=%d stride=%d input_format=%s colorimetry=%s legacy_input_hdr10=%d legacy_hdr10=%d "
              "dma_fd=%d,%d planes=%u w=%u h=%u",
              inbuf->buf_type, inbuf->buf_fmt, inbuf->buf_stride,
              sbs_pixel_format_name(enc->input_format),
              sbs_colorimetry_name(enc->colorimetry),
              enc->input_hdr10 ? 1 : 0, enc->hdr10 ? 1 : 0,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[0] : -1,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[1] : -1,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.num_planes : 0,
              msg->width, msg->height);
    }

    pending_frame_push(enc, msg, request_idr, owned_input, owned_input_size);
    owned_input = NULL;
    sync_dmabuf_write_end(sync_fd);
    sync_dmabuf_read(sync_fd, true);
    meta = enc->encode_fn(enc->handle, frame_type, enc->outbuf, inbuf, &retbuf);
    sync_dmabuf_read(sync_fd, false);

    if (!meta.is_valid) {
        if (meta.err_cod == -ENOSYS && meta.input_frame_num < 0) {
            sbs_pending_frame_t *pending = g_queue_pop_tail(enc->pending_frames);
            pending_frame_free(pending);
            if (enc->next_submit_id > 0)
                enc->next_submit_id--;
            return SBS_ERR_WOULD_BLOCK;
        }

        sbs_pending_frame_t *pending = g_queue_pop_tail(enc->pending_frames);
        pending_frame_free(pending);
        LOG_E("vl_multi_encoder_encode failed: err=%d input_frame_num=%d", meta.err_cod, meta.input_frame_num);
        return SBS_ERR_IO;
    }

    if (meta.encoded_data_length_in_bytes <= 0)
    {
        if (!enc->bframe_enabled) {
            sbs_pending_frame_t *pending = pending_frame_take_match(enc, meta.input_frame_num);
            pending_frame_free(pending);
        } else {
            pending_frame_prune(enc);
        }
        return SBS_OK;
    }

    packet->data = enc->outbuf;
    packet->size = (size_t)meta.encoded_data_length_in_bytes;

    sbs_pending_frame_t *pending = pending_frame_take_match(enc, meta.input_frame_num);
    if (pending) {
        uint64_t delay_ns = bframe_delay_ns(enc);
        packet->pts_ns = enc->bframe_enabled
            ? (pending->pts_ns != UINT64_MAX
                ? pending->pts_ns + delay_ns
                : ((uint64_t)pending->id * enc->frame_duration_ns) + delay_ns)
            : pending->pts_ns;
        packet->dts_ns = calculate_packet_dts(enc, pending, packet->pts_ns);
        packet->duration_ns = pending->duration_ns;
        packet->is_keyframe = pending->requested_idr;
        pending_frame_free(pending);
    } else {
        packet->pts_ns = 0;
        packet->dts_ns = UINT64_MAX;
        packet->duration_ns = 0;
        packet->is_keyframe = request_idr;
    }

    /* Keyframe detection: use extra.frame_type like the amlvenc plugin does.
     * The firmware's is_key_frame field is unreliable (always 0). */
    packet->is_keyframe = packet->is_keyframe ||
                           meta.extra.frame_type == FRAME_TYPE_IDR ||
                           meta.extra.frame_type == FRAME_TYPE_I;

    if (sbs_log_profile_enabled() &&
        (enc->output_counter < 3 || packet->is_keyframe || getenv("SBS_VENC_LOG_PARAMS"))) {
        LOG_I("VENC OUTPUT CONTRACT: output=%lu request_idr=%d request_frame_type=%d meta={len=%d key=%d ts_us=%d input_frame_num=%d extra_type=%d qp=%d intra=%d merged=%d skipped=%d} retbuf={type=%s/%d fmt=%s/%d stride=%d ptr0=0x%lx dma_fd=%d,%d,%d dma_planes=%u} packet={size=%zu key=%d pts=%lu dts=%lu dur=%lu}",
              (unsigned long)enc->output_counter,
              request_idr,
              frame_type,
              meta.encoded_data_length_in_bytes,
              meta.is_key_frame,
              meta.timestamp_us,
              meta.input_frame_num,
              meta.extra.frame_type,
              meta.extra.average_qp_value,
              meta.extra.intra_blocks,
              meta.extra.merged_blocks,
              meta.extra.skipped_blocks,
              buffer_type_name(retbuf.buf_type), retbuf.buf_type,
              img_format_name(retbuf.buf_fmt), retbuf.buf_fmt,
              retbuf.buf_stride,
              retbuf.buf_type == VMALLOC_TYPE ? retbuf.buf_info.in_ptr[0] : 0ul,
              retbuf.buf_type == DMA_TYPE ? retbuf.buf_info.dma_info.shared_fd[0] : -1,
              retbuf.buf_type == DMA_TYPE ? retbuf.buf_info.dma_info.shared_fd[1] : -1,
              retbuf.buf_type == DMA_TYPE ? retbuf.buf_info.dma_info.shared_fd[2] : -1,
              retbuf.buf_type == DMA_TYPE ? retbuf.buf_info.dma_info.num_planes : 0,
              packet->size,
              packet->is_keyframe,
              (unsigned long)packet->pts_ns,
              (unsigned long)packet->dts_ns,
              (unsigned long)packet->duration_ns);
        if (enc->codec_id == CODEC_ID_H265)
            log_h265_nal_summary(packet->data, packet->size);
    }

    enc->output_counter++;

    return SBS_OK;
}

/* ---- Public API ---- */

sbs_direct_venc_t *sbs_direct_venc_new(const sbs_direct_venc_config_t *config)
{
    if (!config || config->width == 0 || config->height == 0)
        return NULL;

    sbs_direct_venc_t *enc = g_new0(sbs_direct_venc_t, 1);
    enc->codec_id = codec_id_from_string(config->codec);
    enc->width = config->width;
    enc->height = config->height;
    enc->fps_num = config->fps_num;
    enc->fps_den = config->fps_den > 0 ? config->fps_den : 1;
    enc->bitrate_kbps = config->bitrate_kbps > 0 ? config->bitrate_kbps : 10000;
    enc->gop_size = config->gop_size;
    enc->gop_pattern = config->gop_pattern;
    enc->rc_mode = config->rc_mode;
    enc->input_format = config->input_format;
    enc->colorimetry = config->colorimetry;
    if ((config->input_hdr10 || config->hdr10) &&
        enc->input_format == SBS_PIXEL_FORMAT_NV21 &&
        enc->colorimetry == SBS_COLORIMETRY_SDR) {
        enc->input_format = SBS_PIXEL_FORMAT_P010;
        enc->colorimetry = SBS_COLORIMETRY_BT2020_PQ;
    }
    enc->hdr10 = enc->colorimetry == SBS_COLORIMETRY_BT2020_PQ;
    enc->input_hdr10 = enc->input_format == SBS_PIXEL_FORMAT_P010;
    enc->bframe_enabled = gop_pattern_has_bframes(enc->gop_pattern);
    enc->frame_duration_ns = calculate_frame_duration_ns(enc->fps_num, enc->fps_den);
    enc->outbuf_size = choose_outbuf_size(config->width, config->height);
    enc->outbuf = g_malloc0(enc->outbuf_size);
    enc->pending_frames = g_queue_new();

    if (!enc->outbuf || load_symbols(enc) != SBS_OK || init_encoder_handle(enc) != SBS_OK) {
        sbs_direct_venc_free(enc);
        return NULL;
    }

    LOG_I("direct encoder ready: codec=%s %ux%u bitrate=%ukbps gop=%u gop_pattern=%d rc_mode=%d input_format=%s colorimetry=%s outbuf=%zuKB",
          codec_id_name(enc->codec_id),
          enc->width, enc->height, enc->bitrate_kbps,
          enc->gop_size, enc->gop_pattern, enc->rc_mode,
          sbs_pixel_format_name(enc->input_format),
          sbs_colorimetry_name(enc->colorimetry),
          enc->outbuf_size / 1024u);
    return enc;
}

void sbs_direct_venc_free(sbs_direct_venc_t *enc)
{
    if (!enc)
        return;

    if (enc->handle != 0 && enc->destroy_fn)
        enc->destroy_fn(enc->handle);
    if (enc->pending_frames)
        g_queue_free_full(enc->pending_frames, pending_frame_free);
    g_free(enc->outbuf);
    if (enc->libvpcodec)
        dlclose(enc->libvpcodec);
    g_free(enc);
}

uint64_t sbs_direct_venc_reorder_delay_ns(const sbs_direct_venc_t *enc)
{
    return bframe_delay_ns(enc);
}

int sbs_direct_venc_submit_dmabuf(sbs_direct_venc_t *enc,
                                   const sbs_video_frame_msg_t *msg,
                                   int dmabuf_fd,
                                   size_t size,
                                   bool force_idr,
                                   sbs_direct_venc_packet_t *packet)
{
    vl_buffer_info_t inbuf;

    (void)size;

    if (!enc || !msg || dmabuf_fd < 0)
        return SBS_ERR_INVAL;

    if (msg->drm_format != 0) {
        bool msg_is_p010 = (msg->drm_format == DRM_FORMAT_P010);
        bool expect_p010 = enc->input_format == SBS_PIXEL_FORMAT_P010;
        if (msg_is_p010 != expect_p010) {
            LOG_W("dropping dmabuf frame: format mismatch (msg_drm=0x%x input_format_expected=%s colorimetry=%s)",
                  (unsigned)msg->drm_format,
                  sbs_pixel_format_name(enc->input_format),
                  sbs_colorimetry_name(enc->colorimetry));
            return SBS_ERR_INVAL;
        }
    }

    memset(&inbuf, 0, sizeof(inbuf));
    inbuf.buf_type = DMA_TYPE;
    inbuf.buf_fmt = enc->input_format == SBS_PIXEL_FORMAT_P010 ? IMG_FMT_P010 : IMG_FMT_NV21;
    inbuf.buf_stride = (int)(msg->plane_stride[0] > 0
        ? msg->plane_stride[0]
        : (enc->input_format == SBS_PIXEL_FORMAT_P010 ? msg->width * 2u : msg->width));
    inbuf.buf_info.dma_info.shared_fd[0] = dmabuf_fd;
    /* Native SDR/HDR export uses one contiguous codecmm DMA-BUF with Y
     * followed by interleaved UV. libvpcodec's DMA path accepts this as a
     * single-plane YUV buffer and resolves chroma addresses from stride/format. */
    inbuf.buf_info.dma_info.shared_fd[1] = -1;
    inbuf.buf_info.dma_info.shared_fd[2] = -1;
    inbuf.buf_info.dma_info.num_planes = 1u;

    log_input_contract(enc, "dmabuf", msg, &inbuf, size);

    return submit_common(enc, msg, &inbuf, dmabuf_fd, NULL, 0,
                         force_idr, packet);
}
