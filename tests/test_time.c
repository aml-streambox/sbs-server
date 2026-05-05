/*
 * SBS - StreamBox Broadcast System
 * Unit tests for time utilities
 */
#include "sbs_test.h"
#include "sbs/time_utils.h"

#include <unistd.h>

SBS_TEST(time, now_us_is_nonzero) {
    sbs_time_us_t t = sbs_time_now_us();
    SBS_ASSERT_GT(t, (sbs_time_us_t)0);
}

SBS_TEST(time, now_us_is_monotonic) {
    sbs_time_us_t t1 = sbs_time_now_us();
    sbs_time_us_t t2 = sbs_time_now_us();
    SBS_ASSERT_GE(t2, t1);
}

SBS_TEST(time, elapsed_measures_delay) {
    sbs_time_us_t start = sbs_time_now_us();
    usleep(10000); /* 10ms */
    uint64_t elapsed = sbs_time_elapsed_us(start);
    /* Should be at least 5ms (some slack for scheduling) */
    SBS_ASSERT_GT(elapsed, (uint64_t)5000);
}

SBS_TEST(time, format_iso8601) {
    char buf[32];
    char *result = sbs_time_format_iso8601(buf, sizeof(buf));
    SBS_ASSERT_NOT_NULL(result);
    SBS_ASSERT_EQ(result, buf);
    /* Verify basic format: YYYY-MM-DDTHH:MM:SS.mmmZ */
    SBS_ASSERT_EQ(buf[4], '-');
    SBS_ASSERT_EQ(buf[7], '-');
    SBS_ASSERT_EQ(buf[10], 'T');
    SBS_ASSERT_EQ(buf[13], ':');
}

SBS_TEST(time, format_iso8601_null_buf) {
    SBS_ASSERT_NULL(sbs_time_format_iso8601(NULL, 32));
}

SBS_TEST(time, format_iso8601_small_buf) {
    char buf[5];
    SBS_ASSERT_NULL(sbs_time_format_iso8601(buf, sizeof(buf)));
}

SBS_TEST(time, format_hms) {
    char buf[16];
    char *result = sbs_time_format_hms(buf, sizeof(buf));
    SBS_ASSERT_NOT_NULL(result);
    SBS_ASSERT_EQ(buf[2], ':');
    SBS_ASSERT_EQ(buf[5], ':');
}

SBS_TEST(time, now_ms_matches_us) {
    sbs_time_us_t t_us = sbs_time_now_us();
    uint64_t t_ms = sbs_time_now_ms();
    /* ms should be approximately us/1000 (within 10ms tolerance) */
    uint64_t diff = (t_us / 1000 > t_ms) ? (t_us / 1000 - t_ms) : (t_ms - t_us / 1000);
    SBS_ASSERT_LT(diff, (uint64_t)10);
}

SBS_TEST_MAIN()
