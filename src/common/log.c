/*
 * SBS - StreamBox Broadcast System
 * Logging subsystem
 */
#define _GNU_SOURCE
#include "sbs/log.h"
#include "sbs/types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

#ifdef SBS_HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

/* ── Global logger state ──────────────────────────────────────── */

static sbs_logger_t g_logger = {
    .level        = SBS_LOG_INFO,
    .file         = NULL,
    .use_journald = false,
    .use_color    = true,
    .component    = "sbs",
};

static const char *level_names[] = {
    "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

#ifdef SBS_HAVE_SYSTEMD
static const int level_to_journal_priority[] = {
    LOG_CRIT,    /* FATAL */
    LOG_ERR,     /* ERROR */
    LOG_WARNING, /* WARN  */
    LOG_INFO,    /* INFO  */
    LOG_DEBUG,   /* DEBUG */
    LOG_DEBUG,   /* TRACE */
};
#endif

/* ANSI color codes */
static const char *level_colors[] = {
    "\033[1;31m", /* FATAL - bold red */
    "\033[0;31m", /* ERROR - red */
    "\033[0;33m", /* WARN  - yellow */
    "\033[0;32m", /* INFO  - green */
    "\033[0;36m", /* DEBUG - cyan */
    "\033[0;90m", /* TRACE - dark gray */
};
static const char *color_reset = "\033[0m";

/* ── API implementation ───────────────────────────────────────── */

void sbs_log_init(const char *component, sbs_log_level_t level)
{
    g_logger.component = component ? component : "sbs";
    g_logger.level = level;

    /* Auto-detect journald: if JOURNAL_STREAM is set, we're under systemd */
    const char *journal_stream = getenv("JOURNAL_STREAM");
    if (journal_stream && journal_stream[0]) {
#ifdef SBS_HAVE_SYSTEMD
        g_logger.use_journald = true;
        g_logger.use_color = false; /* journald handles formatting */
#endif
    }

    /* Check if stderr is a terminal */
    if (!g_logger.use_color) {
        /* Already disabled */
    } else {
        /* isatty check could go here; for embedded we default to color on */
    }

    /* Check SBS_LOG_LEVEL environment override */
    const char *env_level = getenv("SBS_LOG_LEVEL");
    if (env_level) {
        g_logger.level = sbs_log_level_from_string(env_level);
    }
}

void sbs_log_set_level(sbs_log_level_t level)
{
    g_logger.level = level;
}

sbs_log_level_t sbs_log_get_level(void)
{
    return g_logger.level;
}

void sbs_log_set_file(FILE *fp)
{
    g_logger.file = fp;
}

sbs_log_level_t sbs_log_level_from_string(const char *str)
{
    if (!str) return SBS_LOG_INFO;
    if (strcasecmp(str, "fatal") == 0) return SBS_LOG_FATAL;
    if (strcasecmp(str, "error") == 0) return SBS_LOG_ERROR;
    if (strcasecmp(str, "warn")  == 0) return SBS_LOG_WARN;
    if (strcasecmp(str, "info")  == 0) return SBS_LOG_INFO;
    if (strcasecmp(str, "debug") == 0) return SBS_LOG_DEBUG;
    if (strcasecmp(str, "trace") == 0) return SBS_LOG_TRACE;
    return SBS_LOG_INFO;
}

const char *sbs_log_level_name(sbs_log_level_t level)
{
    if (level >= 0 && level <= SBS_LOG_TRACE)
        return level_names[level];
    return "UNKNOWN";
}

void sbs_log(sbs_log_level_t level, const char *component,
             const char *resource_id, const char *fmt, ...)
{
    if (level > g_logger.level)
        return;

    va_list args;
    va_start(args, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char time_str[32];
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);

    const char *comp = component ? component : g_logger.component;

    /* journald with structured fields */
#ifdef SBS_HAVE_SYSTEMD
    if (g_logger.use_journald) {
        sd_journal_send(
            "MESSAGE=%s", msg,
            "PRIORITY=%d", level_to_journal_priority[level],
            "SBS_COMPONENT=%s", comp,
            "SBS_RESOURCE=%s", resource_id ? resource_id : "",
            "SBS_LEVEL=%s", level_names[level],
            "SYSLOG_IDENTIFIER=sbs",
            NULL);
    }
#endif

    /* stderr output */
    if (!g_logger.use_journald) {
        if (g_logger.use_color) {
            fprintf(stderr, "%s %s%-5s%s %s%s%s: %s\n",
                    time_str,
                    level_colors[level], level_names[level], color_reset,
                    comp,
                    resource_id ? ":" : "",
                    resource_id ? resource_id : "",
                    msg);
        } else {
            fprintf(stderr, "%s %-5s %s%s%s: %s\n",
                    time_str, level_names[level],
                    comp,
                    resource_id ? ":" : "",
                    resource_id ? resource_id : "",
                    msg);
        }
    }

    /* File output (if configured) */
    if (g_logger.file) {
        fprintf(g_logger.file, "%s %-5s %s%s%s: %s\n",
                time_str, level_names[level],
                comp,
                resource_id ? ":" : "",
                resource_id ? resource_id : "",
                msg);
        fflush(g_logger.file);
    }
}
