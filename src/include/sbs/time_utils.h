/*
 * SBS - StreamBox Broadcast System
 * Time utilities header
 */
#ifndef SBS_TIME_UTILS_H
#define SBS_TIME_UTILS_H

#include "sbs/types.h"

/**
 * Get current monotonic time in microseconds.
 */
sbs_time_us_t sbs_time_now_us(void);

/**
 * Get current monotonic time in milliseconds.
 */
static inline uint64_t sbs_time_now_ms(void)
{
    return sbs_time_now_us() / 1000;
}

/**
 * Get elapsed time in microseconds since `start`.
 */
static inline uint64_t sbs_time_elapsed_us(sbs_time_us_t start)
{
    return sbs_time_now_us() - start;
}

/**
 * Get elapsed time in milliseconds since `start`.
 */
static inline uint64_t sbs_time_elapsed_ms(sbs_time_us_t start)
{
    return sbs_time_elapsed_us(start) / 1000;
}

/**
 * Format a wall-clock timestamp as ISO 8601 into `buf` (min 32 bytes).
 * Returns buf on success, NULL on error.
 */
char *sbs_time_format_iso8601(char *buf, size_t len);

/**
 * Format a wall-clock timestamp as HH:MM:SS.mmm into `buf` (min 16 bytes).
 * Returns buf on success, NULL on error.
 */
char *sbs_time_format_hms(char *buf, size_t len);

#endif /* SBS_TIME_UTILS_H */
