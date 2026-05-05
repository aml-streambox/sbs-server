/*
 * SBS - StreamBox Broadcast System
 * Minimal single-header test framework for embedded C
 *
 * Usage:
 *   #include "sbs_test.h"
 *
 *   SBS_TEST(suite_name, test_name) {
 *       SBS_ASSERT_EQ(1 + 1, 2);
 *   }
 *
 *   SBS_TEST_MAIN()
 */
#ifndef SBS_TEST_H
#define SBS_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Types ─────────────────────────────────────────────────── */

typedef void (*sbs_test_fn)(void);

typedef struct sbs_test_case {
    const char      *name;
    const char      *suite;
    sbs_test_fn      fn;
    sbs_test_fn      setup;
    sbs_test_fn      teardown;
} sbs_test_case_t;

typedef struct sbs_test_result {
    const char      *name;
    const char      *suite;
    bool             passed;
    const char      *fail_file;
    int              fail_line;
    const char      *fail_expr;
    double           elapsed_ms;
} sbs_test_result_t;

/* ── Global State (single TU) ──────────────────────────────── */

#define SBS_TEST_MAX_CASES   512
#define SBS_TEST_MAX_RESULTS 512

static sbs_test_case_t   g_tests[SBS_TEST_MAX_CASES];
static int                g_test_count = 0;
static sbs_test_result_t  g_results[SBS_TEST_MAX_RESULTS];
static int                g_result_count = 0;
static const char        *g_current_test = NULL;
static bool               g_current_passed = true;
static const char        *g_fail_file = NULL;
static int                g_fail_line = 0;
static const char        *g_fail_expr = NULL;

/* ── Registration ──────────────────────────────────────────── */

#define SBS_TEST(suite_, name_)                                         \
    static void test_##suite_##_##name_(void);                          \
    __attribute__((constructor))                                         \
    static void register_##suite_##_##name_(void) {                     \
        g_tests[g_test_count++] = (sbs_test_case_t){                    \
            .name = #name_, .suite = #suite_,                           \
            .fn = test_##suite_##_##name_,                              \
        };                                                              \
    }                                                                   \
    static void test_##suite_##_##name_(void)

#define SBS_TEST_FIXTURE(suite_, name_, setup_fn, teardown_fn)          \
    static void test_##suite_##_##name_(void);                          \
    __attribute__((constructor))                                         \
    static void register_##suite_##_##name_(void) {                     \
        g_tests[g_test_count++] = (sbs_test_case_t){                    \
            .name = #name_, .suite = #suite_,                           \
            .fn = test_##suite_##_##name_,                              \
            .setup = setup_fn, .teardown = teardown_fn,                 \
        };                                                              \
    }                                                                   \
    static void test_##suite_##_##name_(void)

/* ── Assertions ────────────────────────────────────────────── */

#define SBS_ASSERT(expr)                                                \
    do {                                                                \
        if (!(expr)) {                                                  \
            g_current_passed = false;                                   \
            g_fail_file = __FILE__;                                     \
            g_fail_line = __LINE__;                                     \
            g_fail_expr = #expr;                                        \
            return;                                                     \
        }                                                               \
    } while (0)

#define SBS_ASSERT_EQ(a, b)       SBS_ASSERT((a) == (b))
#define SBS_ASSERT_NE(a, b)       SBS_ASSERT((a) != (b))
#define SBS_ASSERT_GT(a, b)       SBS_ASSERT((a) > (b))
#define SBS_ASSERT_LT(a, b)       SBS_ASSERT((a) < (b))
#define SBS_ASSERT_GE(a, b)       SBS_ASSERT((a) >= (b))
#define SBS_ASSERT_LE(a, b)       SBS_ASSERT((a) <= (b))
#define SBS_ASSERT_NULL(p)        SBS_ASSERT((p) == NULL)
#define SBS_ASSERT_NOT_NULL(p)    SBS_ASSERT((p) != NULL)

#define SBS_ASSERT_STR_EQ(a, b)                                         \
    SBS_ASSERT(strcmp((a), (b)) == 0)

#define SBS_ASSERT_STR_NE(a, b)                                         \
    SBS_ASSERT(strcmp((a), (b)) != 0)

#define SBS_ASSERT_MEM_EQ(a, b, n)                                      \
    SBS_ASSERT(memcmp((a), (b), (n)) == 0)

/* ── Runner ────────────────────────────────────────────────── */

static int sbs_test_run_all(void)
{
    int passed = 0, failed = 0;
    printf("Running %d tests...\n\n", g_test_count);

    for (int i = 0; i < g_test_count; i++) {
        sbs_test_case_t *tc = &g_tests[i];
        g_current_test = tc->name;
        g_current_passed = true;
        g_fail_file = NULL;
        g_fail_line = 0;
        g_fail_expr = NULL;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (tc->setup) tc->setup();
        tc->fn();
        if (tc->teardown) tc->teardown();

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) * 1000.0
                        + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        g_results[g_result_count++] = (sbs_test_result_t){
            .name = tc->name, .suite = tc->suite,
            .passed = g_current_passed,
            .fail_file = g_fail_file, .fail_line = g_fail_line,
            .fail_expr = g_fail_expr, .elapsed_ms = elapsed,
        };

        if (g_current_passed) {
            printf("  PASS  %s.%s (%.1fms)\n", tc->suite, tc->name, elapsed);
            passed++;
        } else {
            printf("  FAIL  %s.%s (%.1fms)\n", tc->suite, tc->name, elapsed);
            printf("        %s:%d: %s\n", g_fail_file, g_fail_line, g_fail_expr);
            failed++;
        }
    }

    printf("\n%d passed, %d failed, %d total\n", passed, failed, g_test_count);
    return failed > 0 ? 1 : 0;
}

/* ── JUnit XML Output ──────────────────────────────────────── */

static void sbs_test_write_junit(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;

    int failures = 0;
    double total_time = 0;
    for (int i = 0; i < g_result_count; i++) {
        if (!g_results[i].passed) failures++;
        total_time += g_results[i].elapsed_ms;
    }

    fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(fp, "<testsuites tests=\"%d\" failures=\"%d\" time=\"%.3f\">\n",
            g_result_count, failures, total_time / 1000.0);
    fprintf(fp, "  <testsuite name=\"sbs\" tests=\"%d\" failures=\"%d\">\n",
            g_result_count, failures);

    for (int i = 0; i < g_result_count; i++) {
        sbs_test_result_t *r = &g_results[i];
        fprintf(fp, "    <testcase classname=\"%s\" name=\"%s\" time=\"%.3f\"",
                r->suite, r->name, r->elapsed_ms / 1000.0);
        if (r->passed) {
            fprintf(fp, " />\n");
        } else {
            fprintf(fp, ">\n");
            fprintf(fp, "      <failure message=\"%s:%d: %s\" />\n",
                    r->fail_file, r->fail_line, r->fail_expr);
            fprintf(fp, "    </testcase>\n");
        }
    }

    fprintf(fp, "  </testsuite>\n");
    fprintf(fp, "</testsuites>\n");
    fclose(fp);
}

/* ── Main Macro ────────────────────────────────────────────── */

#define SBS_TEST_MAIN()                                                 \
    int main(int argc, char **argv) {                                   \
        (void)argc; (void)argv;                                         \
        int ret = sbs_test_run_all();                                   \
        const char *junit = NULL;                                       \
        for (int i = 1; i < argc; i++) {                                \
            if (strncmp(argv[i], "--junit=", 8) == 0)                   \
                junit = argv[i] + 8;                                    \
        }                                                               \
        if (junit) sbs_test_write_junit(junit);                         \
        return ret;                                                     \
    }

#endif /* SBS_TEST_H */
