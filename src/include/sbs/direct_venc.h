#ifndef SBS_DIRECT_VENC_H
#define SBS_DIRECT_VENC_H

#include "sbs/ipc.h"
#include "sbs/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sbs_direct_venc sbs_direct_venc_t;

typedef struct sbs_direct_venc_config {
    const char *codec;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t bitrate_kbps;
    uint32_t gop_size;
    int32_t gop_pattern;
    int32_t rc_mode;        /* 0=VBR (default), 1=CBR */
    bool hdr10;
} sbs_direct_venc_config_t;

typedef struct sbs_direct_venc_packet {
    const uint8_t *data;
    size_t size;
    uint64_t pts_ns;
    uint64_t dts_ns;
    uint64_t duration_ns;
    bool is_keyframe;
} sbs_direct_venc_packet_t;

sbs_direct_venc_t *sbs_direct_venc_new(const sbs_direct_venc_config_t *config);
void sbs_direct_venc_free(sbs_direct_venc_t *enc);
uint64_t sbs_direct_venc_reorder_delay_ns(const sbs_direct_venc_t *enc);

int sbs_direct_venc_submit_dmabuf(sbs_direct_venc_t *enc,
                                  const sbs_video_frame_msg_t *msg,
                                  int dmabuf_fd,
                                  size_t size,
                                  bool force_idr,
                                  sbs_direct_venc_packet_t *packet);

#endif
