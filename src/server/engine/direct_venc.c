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
#include <sys/mman.h>
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
    bool hdr10;
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

    if (!enc->bframe_enabled || enc->frame_duration_ns == 0)
        return pending->dts_ns;

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
    info.img_format = enc->hdr10 ? IMG_FMT_P010 : IMG_FMT_NV21;
    info.enc_feature_opts |= 0x1;
    info.internal_bit_depth = enc->hdr10 ? 10 : 8;
    info.gop_pattern = enc->gop_pattern;
    info.rc_mode = enc->rc_mode;
    info.bitstream_buf_sz_kb = choose_bitstream_buf_sz_kb(enc->width, enc->height);

    info.vui_parameters_present_flag = 1;
    info.video_signal_type_present_flag = 1;
    info.video_full_range_flag = 0;
    info.colour_description_present_flag = 1;
    if (enc->hdr10) {
        info.colour_primaries = 9;
        info.transfer_characteristics = 16;
        info.matrix_coefficients = 9;
    } else {
        info.colour_primaries = 1;
        info.transfer_characteristics = 1;
        info.matrix_coefficients = 1;
    }

    qp.qp_min = 0;
    qp.qp_max = 51;
    qp.qp_I_base = 30;
    qp.qp_I_min = 0;
    qp.qp_I_max = 51;
    qp.qp_P_base = 30;
    qp.qp_P_min = 0;
    qp.qp_P_max = 51;

    enc->handle = enc->init_fn(enc->codec_id, info, &qp);
    if (enc->handle == 0) {
        LOG_E("vl_multi_encoder_init failed: codec=%s %ux%u bitrate=%ukbps gop=%u gop_pattern=%d",
              enc->codec_id == CODEC_ID_H264 ? "h264" : "h265",
              enc->width, enc->height, enc->bitrate_kbps, enc->gop_size, enc->gop_pattern);
        return SBS_ERR_IO;
    }

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

    if (enc->next_submit_id <= 2) {
        LOG_I("submit: buf_type=%d buf_fmt=%d stride=%d hdr10=%d "
              "dma_fd=%d,%d planes=%u w=%u h=%u",
              inbuf->buf_type, inbuf->buf_fmt, inbuf->buf_stride, enc->hdr10,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[0] : -1,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.shared_fd[1] : -1,
              inbuf->buf_type == DMA_TYPE ? inbuf->buf_info.dma_info.num_planes : 0,
              msg->width, msg->height);
    }

    if (enc->next_submit_id <= 4) {
        if (inbuf->buf_type == VMALLOC_TYPE && inbuf->buf_info.in_ptr[0]) {
            if (enc->hdr10) {
                const uint16_t *p = (const uint16_t *)(unsigned long)inbuf->buf_info.in_ptr[0];
                uint32_t stride_u16 = inbuf->buf_stride / 2;
                const uint16_t *row100 = p + stride_u16 * 100;
                const uint16_t *row540 = p + stride_u16 * 540;
                LOG_I("P010 VMALLOC row0[0..3]=%u %u %u %u row100[0..3]=%u %u %u %u row540[0..3]=%u %u %u %u stride=%d",
                      p[0], p[1], p[2], p[3],
                      row100[0], row100[1], row100[2], row100[3],
                      row540[0], row540[1], row540[2], row540[3],
                      inbuf->buf_stride);
                uint32_t uv_off = msg->plane_offset[1] > 0 ? msg->plane_offset[1] : msg->width * msg->height * 2;
                const uint16_t *uv = (const uint16_t *)(unsigned long)(inbuf->buf_info.in_ptr[0] + uv_off);
                const uint16_t *uv270 = uv + stride_u16 * 270;
                LOG_I("P010 VMALLOC UV row0[0..3]=%u %u %u %u row270[0..3]=%u %u %u %u uv_off=%u",
                      uv[0], uv[1], uv[2], uv[3],
                      uv270[0], uv270[1], uv270[2], uv270[3],
                      uv_off);
            } else {
                const uint8_t *p = (const uint8_t *)(unsigned long)inbuf->buf_info.in_ptr[0];
                LOG_I("NV21 VMALLOC row0[0..3]=%u %u %u %u row100[0..3]=%u %u %u %u stride=%d",
                      p[0], p[1], p[2], p[3],
                      p[inbuf->buf_stride * 100], p[inbuf->buf_stride * 100 + 1], p[inbuf->buf_stride * 100 + 2], p[inbuf->buf_stride * 100 + 3],
                      inbuf->buf_stride);
            }
        }
    }

    pending_frame_push(enc, msg, request_idr, owned_input, owned_input_size);
    owned_input = NULL;
    sync_dmabuf_write_end(sync_fd);
    sync_dmabuf_read(sync_fd, true);
    meta = enc->encode_fn(enc->handle, frame_type, enc->outbuf, inbuf, &retbuf);
    sync_dmabuf_read(sync_fd, false);

    if (!meta.is_valid) {
        if (enc->bframe_enabled && meta.err_cod == -ENOSYS && meta.input_frame_num < 0) {
            pending_frame_prune(enc);
            return SBS_OK;
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
    enc->bitrate_kbps = config->bitrate_kbps > 0 ? config->bitrate_kbps : 20000;
    enc->gop_size = config->gop_size;
    enc->gop_pattern = config->gop_pattern;
    enc->rc_mode = config->rc_mode;
    enc->hdr10 = config->hdr10;
    enc->bframe_enabled = gop_pattern_has_bframes(enc->gop_pattern);
    enc->frame_duration_ns = calculate_frame_duration_ns(enc->fps_num, enc->fps_den);
    enc->outbuf_size = choose_outbuf_size(config->width, config->height);
    enc->outbuf = g_malloc0(enc->outbuf_size);
    enc->pending_frames = g_queue_new();

    if (!enc->outbuf || load_symbols(enc) != SBS_OK || init_encoder_handle(enc) != SBS_OK) {
        sbs_direct_venc_free(enc);
        return NULL;
    }

    LOG_I("direct encoder ready: codec=%s %ux%u bitrate=%ukbps gop=%u gop_pattern=%d rc_mode=%d hdr10=%d outbuf=%zuKB",
          enc->codec_id == CODEC_ID_H264 ? "h264" : "h265",
          enc->width, enc->height, enc->bitrate_kbps,
          enc->gop_size, enc->gop_pattern, enc->rc_mode, enc->hdr10,
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

int sbs_direct_venc_submit_ptr(sbs_direct_venc_t *enc,
                                const sbs_video_frame_msg_t *msg,
                                const void *data,
                                size_t size,
                                bool force_idr,
                                sbs_direct_venc_packet_t *packet)
{
    vl_buffer_info_t inbuf;

    if (!enc || !msg || !data || size == 0)
        return SBS_ERR_INVAL;

    /* Guard against format mismatch: drop frames that don't match the
     * encoder's current hdr10 config. This can happen during a color_mode
     * transition where an in-flight frame (from the old format's slot)
     * arrives after the encoder has been reconfigured for the new format.
     * Reading past the end of an SDR (12MB) slot as if it were P010 (33MB)
     * causes a segfault in the encoder. */
    if (msg->drm_format != 0) {
        bool msg_is_p010 = (msg->drm_format == DRM_FORMAT_P010);
        if (msg_is_p010 != enc->hdr10) {
            LOG_W("dropping frame: format mismatch (msg_drm=0x%x hdr10_expected=%d)",
                  (unsigned)msg->drm_format, enc->hdr10);
            return SBS_ERR_INVAL;
        }
    }
    /* Also sanity-check the buffer size against expected P010/NV21 frame size */
    {
        size_t expected_y = (size_t)msg->width * msg->height * (enc->hdr10 ? 2 : 1);
        size_t expected_uv = (size_t)msg->width * msg->height * (enc->hdr10 ? 2 : 1) / (enc->hdr10 ? 1 : 2);
        size_t expected_total = expected_y + expected_uv;
        if (size < expected_total) {
            LOG_W("dropping frame: buffer too small (size=%zu expected>=%zu hdr10=%d w=%u h=%u)",
                  size, expected_total, enc->hdr10, msg->width, msg->height);
            return SBS_ERR_INVAL;
        }
    }

    uint8_t *owned_input = NULL;
    size_t owned_input_size = 0;
    const void *submit_data = data;

    if (enc->bframe_enabled) {
        owned_input = g_malloc(size);
        if (!owned_input)
            return SBS_ERR_NOMEM;
        memcpy(owned_input, data, size);
        owned_input_size = size;
        submit_data = owned_input;
    }

    memset(&inbuf, 0, sizeof(inbuf));
    inbuf.buf_type = VMALLOC_TYPE;
    inbuf.buf_fmt = enc->hdr10 ? IMG_FMT_P010 : IMG_FMT_NV21;
    inbuf.buf_stride = (int)(msg->plane_stride[0] > 0 ? msg->plane_stride[0] : (enc->hdr10 ? msg->width * 2 : msg->width));
    inbuf.buf_info.in_ptr[0] = (unsigned long)submit_data;
    if (enc->hdr10) {
        /* The Wave521 P010 VMALLOC path only handles one contiguous Y+UV
         * buffer. Supplying a separate UV pointer selects the broken
         * multi-plane path and can stall/fail 4K input. */
        inbuf.buf_info.in_ptr[1] = 0;
    } else {
        inbuf.buf_info.in_ptr[1] = 0;
    }
    inbuf.buf_info.in_ptr[2] = 0;

    return submit_common(enc, msg, &inbuf, -1, owned_input, owned_input_size,
                         force_idr, packet);
}

int sbs_direct_venc_submit_dmabuf(sbs_direct_venc_t *enc,
                                   const sbs_video_frame_msg_t *msg,
                                   int dmabuf_fd,
                                   size_t size,
                                   bool force_idr,
                                   sbs_direct_venc_packet_t *packet)
{
    void *mapped;
    int rc;

    if (!enc || !msg || dmabuf_fd < 0)
        return SBS_ERR_INVAL;

    if (msg->drm_format != 0) {
        bool msg_is_p010 = (msg->drm_format == DRM_FORMAT_P010);
        if (msg_is_p010 != enc->hdr10) {
            LOG_W("dropping dmabuf frame: format mismatch (msg_drm=0x%x hdr10_expected=%d)",
                  (unsigned)msg->drm_format, enc->hdr10);
            return SBS_ERR_INVAL;
        }
    }

    /* libvpcodec only accepts DMA_TYPE for RGB/RGBA input on this platform.
     * Native exports are NV21/P010, so map the single contiguous DMA-BUF and
     * submit through the VMALLOC path to avoid the broken YUV DMA path. */
    sync_dmabuf_write_end(dmabuf_fd);
    sync_dmabuf_read(dmabuf_fd, true);
    mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, dmabuf_fd, 0);
    if (mapped == MAP_FAILED) {
        sync_dmabuf_read(dmabuf_fd, false);
        LOG_E("dmabuf mmap for VMALLOC submit failed: %s", strerror(errno));
        return SBS_ERR_IO;
    }

    rc = sbs_direct_venc_submit_ptr(enc, msg, mapped, size, force_idr, packet);
    munmap(mapped, size);
    sync_dmabuf_read(dmabuf_fd, false);
    return rc;
}
