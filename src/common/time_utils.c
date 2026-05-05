/*
 * SBS - StreamBox Broadcast System
 * Time utilities
 */
#define _GNU_SOURCE
#include "sbs/time_utils.h"
#include "sbs/types.h"

#include <time.h>
#include <stdio.h>

sbs_time_us_t sbs_time_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (sbs_time_us_t)ts.tv_sec * 1000000ULL + (sbs_time_us_t)ts.tv_nsec / 1000ULL;
}

char *sbs_time_format_iso8601(char *buf, size_t len)
{
    if (!buf || len < 28) return NULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000);

    return buf;
}

char *sbs_time_format_hms(char *buf, size_t len)
{
    if (!buf || len < 13) return NULL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    snprintf(buf, len, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000);

    return buf;
}
