/*
 * SBS - StreamBox Broadcast System
 * Unit tests for logging subsystem
 */
#include "sbs_test.h"
#include "sbs/log.h"

/* ── Log level string parsing ─────────────────────────────────── */

SBS_TEST(log, level_from_string_fatal) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("fatal"), SBS_LOG_FATAL);
}

SBS_TEST(log, level_from_string_error) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("error"), SBS_LOG_ERROR);
}

SBS_TEST(log, level_from_string_warn) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("warn"), SBS_LOG_WARN);
}

SBS_TEST(log, level_from_string_info) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("info"), SBS_LOG_INFO);
}

SBS_TEST(log, level_from_string_debug) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("debug"), SBS_LOG_DEBUG);
}

SBS_TEST(log, level_from_string_trace) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("trace"), SBS_LOG_TRACE);
}

SBS_TEST(log, level_from_string_case_insensitive) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("DEBUG"), SBS_LOG_DEBUG);
    SBS_ASSERT_EQ(sbs_log_level_from_string("Error"), SBS_LOG_ERROR);
}

SBS_TEST(log, level_from_string_unknown_defaults_info) {
    SBS_ASSERT_EQ(sbs_log_level_from_string("garbage"), SBS_LOG_INFO);
}

SBS_TEST(log, level_from_string_null_defaults_info) {
    SBS_ASSERT_EQ(sbs_log_level_from_string(NULL), SBS_LOG_INFO);
}

/* ── Log level name ───────────────────────────────────────────── */

SBS_TEST(log, level_name_valid) {
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_FATAL), "FATAL");
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_ERROR), "ERROR");
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_WARN),  "WARN");
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_INFO),  "INFO");
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_DEBUG), "DEBUG");
    SBS_ASSERT_STR_EQ(sbs_log_level_name(SBS_LOG_TRACE), "TRACE");
}

/* ── Log init and level control ───────────────────────────────── */

SBS_TEST(log, init_sets_level) {
    sbs_log_init("test", SBS_LOG_DEBUG);
    SBS_ASSERT_EQ(sbs_log_get_level(), SBS_LOG_DEBUG);
}

SBS_TEST(log, set_level) {
    sbs_log_set_level(SBS_LOG_WARN);
    SBS_ASSERT_EQ(sbs_log_get_level(), SBS_LOG_WARN);
    /* Restore */
    sbs_log_set_level(SBS_LOG_INFO);
}

SBS_TEST(log, log_does_not_crash) {
    /* Just verify logging doesn't segfault */
    sbs_log_init("test", SBS_LOG_TRACE);
    sbs_log(SBS_LOG_INFO, "test", NULL, "hello %s", "world");
    sbs_log(SBS_LOG_DEBUG, "test", "res-1", "resource log %d", 42);
    sbs_log(SBS_LOG_TRACE, NULL, NULL, "null component");
    sbs_log_set_level(SBS_LOG_INFO); /* Restore */
    SBS_ASSERT(true); /* Reached here = no crash */
}

SBS_TEST_MAIN()
