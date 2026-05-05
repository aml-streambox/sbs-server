/*
 * SBS - StreamBox Broadcast System
 * IPC protocol definitions (Phase 2)
 *
 * Wire format follows document/04-ipc-protocol.md exactly.
 * All structures are packed for deterministic wire layout.
 */
#ifndef SBS_IPC_H
#define SBS_IPC_H

#include "sbs/types.h"
#include <string.h>

/* ── Message Header (8 bytes) ─────────────────────────────────── */

typedef enum sbs_ipc_msg_type {
    /* Data messages (carry DMA-BUF fds via SCM_RIGHTS) */
    SBS_IPC_MSG_VIDEO_FRAME     = 0x0001,  /* Video frame: source → compositor */
    SBS_IPC_MSG_COMPOSED_FRAME  = 0x0002,  /* Composed frame: compositor → output */
    SBS_IPC_MSG_AUDIO_BUFFER    = 0x0003,  /* Audio PCM buffer */

    /* Control messages (no ancillary data) */
    SBS_IPC_MSG_STATUS          = 0x0010,  /* Worker status report */
    SBS_IPC_MSG_CONFIG_UPDATE   = 0x0011,  /* Configuration update (supervisor → worker) */
    SBS_IPC_MSG_SHUTDOWN        = 0x0012,  /* Graceful shutdown request */
    SBS_IPC_MSG_SIGNAL_CHANGE   = 0x0013,  /* HDMI signal changed (source → supervisor) */
    SBS_IPC_MSG_FRAME_RELEASE   = 0x0014,  /* Source frame lease release (supervisor → source) */

    /* Response messages */
    SBS_IPC_MSG_ACK             = 0x0020,  /* Acknowledgment */
    SBS_IPC_MSG_ERROR           = 0x00FF,  /* Error report */
} sbs_ipc_msg_type_t;

typedef struct sbs_ipc_msg_header {
    uint32_t msg_type;       /* sbs_ipc_msg_type_t */
    uint32_t payload_size;   /* Size of payload following this header */
} __attribute__((packed)) sbs_ipc_msg_header_t;

/* ── Video Frame Message (128 bytes) ──────────────────────────── */

/* Frame flags */
#define SBS_FRAME_FLAG_HDR          (1 << 0)  /* HDR content (PQ/HLG transfer) */
#define SBS_FRAME_FLAG_INTERLACED   (1 << 1)  /* Interlaced frame */
#define SBS_FRAME_FLAG_TOP_FIRST    (1 << 2)  /* Top field first (if interlaced) */
#define SBS_FRAME_FLAG_KEYFRAME     (1 << 3)  /* Source indicates keyframe boundary */
#define SBS_FRAME_FLAG_CORRUPTED    (1 << 4)  /* Frame may be corrupted */
#define SBS_FRAME_FLAG_LAST         (1 << 5)  /* Last frame before EOS */
#define SBS_FRAME_FLAG_NEEDS_RELEASE (1 << 6) /* Source must be told when frame can be recycled */

typedef struct sbs_video_frame_msg {
    sbs_ipc_msg_header_t header;     /* msg_type = VIDEO_FRAME or COMPOSED_FRAME */

    /* Timing */
    uint64_t  pts_ns;                /* Presentation timestamp (nanoseconds) */
    uint64_t  dts_ns;                /* Decode timestamp */
    uint64_t  duration_ns;           /* Frame duration */
    uint64_t  sequence;              /* Monotonic frame counter */

    /* Format */
    uint32_t  width;
    uint32_t  height;
    uint32_t  drm_format;            /* DRM_FORMAT_NV12, DRM_FORMAT_ABGR8888, etc. */
    uint64_t  drm_modifier;          /* DRM_FORMAT_MOD_LINEAR or vendor-specific */

    /* Planes */
    uint32_t  n_planes;              /* Number of backing fds/planes; P010 output uses 1 contiguous fd */
    uint32_t  plane_offset[4];       /* Byte offset of each plane */
    uint32_t  plane_stride[4];       /* Row stride in bytes */

    /* Backing buffer type */
    uint32_t  buffer_type;           /* sbs_frame_buffer_type_t */

    /* Flags */
    uint32_t  flags;                 /* SBS_FRAME_FLAG_* bitmask */

    /* Second DMA-BUF fd for multi-planar linear formats (e.g. libvfmcap NV12/P010).
     * Set to -1 when not used.  Transport layer sends this as a second SCM_RIGHTS fd. */
    int32_t   dmabuf_fd2;
    uint32_t  _reserved[5];
} __attribute__((packed)) sbs_video_frame_msg_t;

/* Compile-time assertion: video frame message must be exactly 128 bytes */
_Static_assert(sizeof(sbs_video_frame_msg_t) == 128,
               "sbs_video_frame_msg_t must be 128 bytes");

/* ── Worker State (for status messages) ───────────────────────── */

typedef enum sbs_worker_state {
    SBS_WORKER_STATE_INIT       = 0,
    SBS_WORKER_STATE_READY      = 1,  /* Pipeline built, ready to produce/consume */
    SBS_WORKER_STATE_RUNNING    = 2,  /* Actively producing/consuming frames */
    SBS_WORKER_STATE_PAUSED     = 3,  /* Pipeline paused */
    SBS_WORKER_STATE_ERROR      = 4,  /* Pipeline error */
    SBS_WORKER_STATE_EOS        = 5,  /* End of stream */
} sbs_worker_state_t;

/* ── Status Message ───────────────────────────────────────────── */

typedef struct sbs_status_msg {
    sbs_ipc_msg_header_t  header;    /* msg_type = SBS_IPC_MSG_STATUS */

    uint32_t  state;                 /* sbs_worker_state_t */

    /* Source-specific metrics */
    uint32_t  frame_width;
    uint32_t  frame_height;
    uint32_t  frame_rate_num;
    uint32_t  frame_rate_den;
    uint64_t  frames_produced;
    uint64_t  frames_dropped;

    /* Output-specific metrics */
    uint64_t  frames_consumed;
    uint64_t  frames_encoded;
    uint64_t  bytes_written;
    uint32_t  encoder_bitrate;       /* Actual bitrate (kbps) */
    uint32_t  encoder_qp;            /* Current QP value */

    /* Error info (when state == ERROR) */
    char      error_message[256];
} __attribute__((packed)) sbs_status_msg_t;

/* ── Audio Buffer Message ──────────────────────────────────────── */

typedef enum sbs_audio_format {
    SBS_AUDIO_FORMAT_S16LE = 0,
} sbs_audio_format_t;

typedef struct sbs_audio_buffer_msg {
    sbs_ipc_msg_header_t header;      /* msg_type = SBS_IPC_MSG_AUDIO_BUFFER */
    uint64_t  pts_ns;
    uint64_t  duration_ns;
    uint32_t  sample_rate;
    uint32_t  channels;
    uint32_t  format;                 /* sbs_audio_format_t */
    uint32_t  n_samples;
    uint32_t  data_size;
    uint32_t  _reserved;
} __attribute__((packed)) sbs_audio_buffer_msg_t;

/* ── Signal Change Message ────────────────────────────────────── */

typedef struct sbs_signal_change_msg {
    sbs_ipc_msg_header_t  header;    /* msg_type = SBS_IPC_MSG_SIGNAL_CHANGE */

    char      reason[64];            /* "signal-change", "signal-lost", "signal-timeout" */

    /* New signal parameters (valid when reason is "signal-change") */
    uint32_t  width;
    uint32_t  height;
    uint32_t  frame_rate_raw;        /* Raw framerate from driver */
    char      color_space[32];       /* "BT.709", "BT.2020", etc. */
    uint32_t  color_depth;           /* 8, 10, 12 */
    char      hdr_eotf[32];         /* "SDR", "PQ", "HLG" */
    uint32_t  dolby_vision;          /* 0 = none, 1 = present */
    uint32_t  interlace;             /* 0 = progressive, 1 = interlaced */
} __attribute__((packed)) sbs_signal_change_msg_t;

typedef struct sbs_frame_release_msg {
    sbs_ipc_msg_header_t header;     /* msg_type = SBS_IPC_MSG_FRAME_RELEASE */
    uint64_t sequence;               /* VIDEO_FRAME sequence to release */
    uint32_t reason;                 /* reserved for future drop/consume reasons */
    uint32_t _reserved;
} __attribute__((packed)) sbs_frame_release_msg_t;

/* ── Shutdown Message ─────────────────────────────────────────── */

typedef struct sbs_shutdown_msg {
    sbs_ipc_msg_header_t  header;    /* msg_type = SBS_IPC_MSG_SHUTDOWN */
    uint32_t  grace_period_ms;       /* Time to finish current operation */
    uint32_t  reason;                /* 0 = normal, 1 = reconfigure, 2 = system shutdown */
} __attribute__((packed)) sbs_shutdown_msg_t;

/* ── Header helpers (implemented in ipc_proto.c) ──────────────── */

/**
 * Initialize a message header.
 */
void sbs_ipc_header_init(sbs_ipc_msg_header_t *hdr, sbs_ipc_msg_type_t type,
                          uint32_t payload_size);

/**
 * Validate a message header (type range check).
 */
bool sbs_ipc_header_valid(const sbs_ipc_msg_header_t *hdr);

/**
 * Initialize a video frame message with zeroed fields.
 */
static inline void sbs_video_frame_msg_init(sbs_video_frame_msg_t *msg,
                                              sbs_ipc_msg_type_t type)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.msg_type = (uint32_t)type;
    msg->header.payload_size = sizeof(*msg) - sizeof(msg->header);
    msg->dmabuf_fd2 = -1;
}

/**
 * Initialize a status message with zeroed fields.
 */
static inline void sbs_status_msg_init(sbs_status_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.msg_type = SBS_IPC_MSG_STATUS;
    msg->header.payload_size = sizeof(*msg) - sizeof(msg->header);
}

/**
 * Initialize a signal change message with zeroed fields.
 */
static inline void sbs_signal_change_msg_init(sbs_signal_change_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.msg_type = SBS_IPC_MSG_SIGNAL_CHANGE;
    msg->header.payload_size = sizeof(*msg) - sizeof(msg->header);
}

static inline void sbs_frame_release_msg_init(sbs_frame_release_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.msg_type = SBS_IPC_MSG_FRAME_RELEASE;
    msg->header.payload_size = sizeof(*msg) - sizeof(msg->header);
}

/**
 * Initialize a shutdown message.
 */
static inline void sbs_shutdown_msg_init(sbs_shutdown_msg_t *msg,
                                          uint32_t grace_ms, uint32_t reason)
{
    memset(msg, 0, sizeof(*msg));
    msg->header.msg_type = SBS_IPC_MSG_SHUTDOWN;
    msg->header.payload_size = sizeof(*msg) - sizeof(msg->header);
    msg->grace_period_ms = grace_ms;
    msg->reason = reason;
}

#endif /* SBS_IPC_H */
