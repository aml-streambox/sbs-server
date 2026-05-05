/*
 * SBS - StreamBox Broadcast System
 * Common types and forward declarations
 */
#ifndef SBS_TYPES_H
#define SBS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* UUID string: 36 chars + null (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) */
#define SBS_UUID_STRING_LEN  37

typedef char sbs_uuid_t[SBS_UUID_STRING_LEN];

/* Timestamp in microseconds (CLOCK_MONOTONIC) */
typedef uint64_t sbs_time_us_t;

/* DMA-BUF file descriptor (or -1 for invalid) */
typedef int sbs_dmabuf_fd_t;

typedef enum sbs_frame_buffer_type {
    SBS_FRAME_BUFFER_MEMFD = 0,
    SBS_FRAME_BUFFER_DMABUF = 1,
} sbs_frame_buffer_type_t;

/* Canvas pixel formats */
typedef enum sbs_pixel_format {
    SBS_PIXEL_FORMAT_NV12 = 0,   /* 8-bit SDR */
    SBS_PIXEL_FORMAT_P010 = 1,   /* 10-bit HDR */
} sbs_pixel_format_t;

/* Canvas resolution */
typedef struct sbs_resolution {
    uint32_t width;
    uint32_t height;
} sbs_resolution_t;

/* Common return code: 0 = success, negative = error */
typedef int sbs_result_t;

/* ── Error codes ──────────────────────────────────────────────── */
#define SBS_OK                  0
#define SBS_ERR_GENERIC        (-1)
#define SBS_ERR_NOMEM          (-2)
#define SBS_ERR_INVAL          (-3)
#define SBS_ERR_IO             (-4)
#define SBS_ERR_WOULD_BLOCK    (-5)  /* Non-blocking op would block (EAGAIN) */
#define SBS_ERR_PIPE           (-6)  /* Broken pipe / peer disconnected */
#define SBS_ERR_EOF            (-7)  /* End of stream / peer closed */
#define SBS_ERR_TIMEOUT        (-8)
#define SBS_ERR_NOT_FOUND      (-9)

#endif /* SBS_TYPES_H */
