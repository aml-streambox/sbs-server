#define SBS_LOG_COMP "api-telemetry"

#include "sbs/telemetry_utils.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_text_file(const char *path, char *buf, size_t len)
{
    FILE *f;

    if (!path || !buf || len == 0)
        return false;

    f = fopen(path, "r");
    if (!f)
        return false;
    if (!fgets(buf, len, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool parse_first_number(const char *text, double *out)
{
    const char *p;
    char *end = NULL;
    double value;

    if (!text || !out)
        return false;

    for (p = text; *p; p++) {
        if (isdigit((unsigned char)*p) || *p == '.')
            break;
    }
    if (!*p)
        return false;

    errno = 0;
    value = strtod(p, &end);
    if (end == p || errno == ERANGE)
        return false;

    if (value < 0.0)
        value = 0.0;
    if (value > 100.0)
        value = 100.0;
    *out = value;
    return true;
}

static bool read_usage_file(const char *path, double *out)
{
    char buf[128];

    if (!read_text_file(path, buf, sizeof(buf)))
        return false;
    return parse_first_number(buf, out);
}

static bool read_mali_dvfs_utilization(double *out)
{
    static bool have_prev = false;
    static uint64_t prev_busy = 0;
    static uint64_t prev_idle = 0;
    char buf[128];
    unsigned long long busy_raw = 0;
    unsigned long long idle_raw = 0;
    uint64_t busy;
    uint64_t idle;
    uint64_t busy_delta;
    uint64_t total_delta;

    if (!out || !read_text_file("/sys/kernel/debug/mali0/dvfs_utilization", buf, sizeof(buf)))
        return false;

    if (sscanf(buf, "busy_time: %llu idle_time: %llu", &busy_raw, &idle_raw) != 2)
        return false;

    busy = (uint64_t)busy_raw;
    idle = (uint64_t)idle_raw;
    if (have_prev && busy >= prev_busy && idle >= prev_idle) {
        busy_delta = busy - prev_busy;
        total_delta = busy_delta + (idle - prev_idle);
    } else {
        busy_delta = busy;
        total_delta = busy + idle;
        have_prev = true;
    }
    prev_busy = busy;
    prev_idle = idle;

    if (total_delta == 0)
        return false;

    *out = ((double)busy_delta / (double)total_delta) * 100.0;
    return true;
}

static bool name_looks_like_gpu(const char *name)
{
    return name && (strstr(name, "gpu") || strstr(name, "mali") || strstr(name, "Mali"));
}

static bool read_devfreq_named_usage(double *out)
{
    static const char *files[] = { "load", "utilization", "gpu_load" };
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/sys/class/devfreq");
    if (!dir)
        return false;

    while ((entry = readdir(dir)) != NULL) {
        char path[512];

        if (!name_looks_like_gpu(entry->d_name))
            continue;

        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            snprintf(path, sizeof(path), "/sys/class/devfreq/%s/%s", entry->d_name, files[i]);
            if (read_usage_file(path, out)) {
                closedir(dir);
                return true;
            }
        }
    }

    closedir(dir);
    return false;
}

double sbs_telemetry_read_gpu_usage(void)
{
    static const char *paths[] = {
        "/sys/class/devfreq/ff9a0000.gpu/load",
        "/sys/class/devfreq/gpu/load",
        "/sys/class/misc/mali0/device/utilization",
        "/sys/class/misc/mali0/device/gpu_utilization",
        "/sys/kernel/debug/mali0/gpu_utilization",
    };
    double usage = 0.0;

    if (read_mali_dvfs_utilization(&usage))
        return usage;

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (read_usage_file(paths[i], &usage))
            return usage;
    }

    if (read_devfreq_named_usage(&usage))
        return usage;

    return 0.0;
}
