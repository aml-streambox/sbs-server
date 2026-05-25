/*
 * SBS - StreamBox Broadcast System
 * Common types and forward declarations
 */
#ifndef SBS_TYPES_H
#define SBS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
    SBS_PIXEL_FORMAT_NV21 = 0,   /* 8-bit 4:2:0 semi-planar */
    SBS_PIXEL_FORMAT_NV12 = SBS_PIXEL_FORMAT_NV21, /* Legacy alias */
    SBS_PIXEL_FORMAT_P010 = 1,   /* 10-bit 4:2:0 semi-planar */
} sbs_pixel_format_t;

typedef enum sbs_colorimetry {
    SBS_COLORIMETRY_SDR = 0,          /* Legacy SDR: BT.709-style VUI */
    SBS_COLORIMETRY_BT601 = 1,
    SBS_COLORIMETRY_BT709 = 2,
    SBS_COLORIMETRY_BT2020_PQ = 3,
    SBS_COLORIMETRY_BT2100_HLG = 4,
} sbs_colorimetry_t;

static inline const char *sbs_pixel_format_name(sbs_pixel_format_t format)
{
    return format == SBS_PIXEL_FORMAT_P010 ? "p010" : "nv21";
}

static inline bool sbs_pixel_format_parse(const char *value,
                                          sbs_pixel_format_t *out_format)
{
    if (!value || !out_format)
        return false;
    if (strcmp(value, "p010") == 0 || strcmp(value, "10bit") == 0 ||
        strcmp(value, "10-bit") == 0 || strcmp(value, "hdr10") == 0) {
        *out_format = SBS_PIXEL_FORMAT_P010;
        return true;
    }
    if (strcmp(value, "nv21") == 0 || strcmp(value, "nv12") == 0 ||
        strcmp(value, "8bit") == 0 || strcmp(value, "8-bit") == 0 ||
        strcmp(value, "sdr") == 0) {
        *out_format = SBS_PIXEL_FORMAT_NV21;
        return true;
    }
    return false;
}

static inline sbs_pixel_format_t sbs_pixel_format_from_legacy_color_mode(const char *color_mode)
{
    return color_mode && strcmp(color_mode, "hdr10") == 0
        ? SBS_PIXEL_FORMAT_P010
        : SBS_PIXEL_FORMAT_NV21;
}

static inline const char *sbs_legacy_color_mode_for_pixel_format(sbs_pixel_format_t format)
{
    return format == SBS_PIXEL_FORMAT_P010 ? "hdr10" : "sdr";
}

static inline const char *sbs_colorimetry_name(sbs_colorimetry_t colorimetry)
{
    switch (colorimetry) {
    case SBS_COLORIMETRY_BT601: return "bt601";
    case SBS_COLORIMETRY_BT709: return "bt709";
    case SBS_COLORIMETRY_BT2020_PQ: return "bt2020_pq";
    case SBS_COLORIMETRY_BT2100_HLG: return "bt2100_hlg";
    case SBS_COLORIMETRY_SDR:
    default:
        return "sdr";
    }
}

static inline bool sbs_colorimetry_parse(const char *value,
                                         sbs_colorimetry_t *out_colorimetry)
{
    if (!value || !out_colorimetry)
        return false;
    if (strcmp(value, "sdr") == 0) {
        *out_colorimetry = SBS_COLORIMETRY_SDR;
        return true;
    }
    if (strcmp(value, "bt601") == 0 || strcmp(value, "bt.601") == 0 ||
        strcmp(value, "rec601") == 0) {
        *out_colorimetry = SBS_COLORIMETRY_BT601;
        return true;
    }
    if (strcmp(value, "bt709") == 0 || strcmp(value, "bt.709") == 0 ||
        strcmp(value, "rec709") == 0) {
        *out_colorimetry = SBS_COLORIMETRY_BT709;
        return true;
    }
    if (strcmp(value, "bt2020_pq") == 0 || strcmp(value, "bt2020-pq") == 0 ||
        strcmp(value, "hdr10") == 0 || strcmp(value, "pq") == 0) {
        *out_colorimetry = SBS_COLORIMETRY_BT2020_PQ;
        return true;
    }
    if (strcmp(value, "bt2100_hlg") == 0 || strcmp(value, "bt2100-hlg") == 0 ||
        strcmp(value, "hlg") == 0) {
        *out_colorimetry = SBS_COLORIMETRY_BT2100_HLG;
        return true;
    }
    return false;
}

static inline sbs_colorimetry_t sbs_colorimetry_from_legacy_color_mode(const char *color_mode)
{
    return color_mode && strcmp(color_mode, "hdr10") == 0
        ? SBS_COLORIMETRY_BT2020_PQ
        : SBS_COLORIMETRY_SDR;
}

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
