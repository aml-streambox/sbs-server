/*
 * SBS - StreamBox Broadcast System
 * Logging subsystem header
 */
#ifndef SBS_LOG_H
#define SBS_LOG_H

#include <stdbool.h>
#include <stdio.h>

/* ── Log Levels ───────────────────────────────────────────────── */

typedef enum sbs_log_level {
    SBS_LOG_FATAL = 0,
    SBS_LOG_ERROR = 1,
    SBS_LOG_WARN  = 2,
    SBS_LOG_INFO  = 3,
    SBS_LOG_DEBUG = 4,
    SBS_LOG_TRACE = 5,
} sbs_log_level_t;

/* ── Logger Configuration ─────────────────────────────────────── */

typedef struct sbs_logger {
    sbs_log_level_t   level;
    FILE             *file;          /* Optional file output (NULL = disabled) */
    bool              use_journald;  /* Send to sd_journal */
    bool              use_color;     /* ANSI colors for stderr */
    const char       *component;     /* "server", "worker", "cli" */
} sbs_logger_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Initialize the global logger.
 * Must be called once before any logging.
 */
void sbs_log_init(const char *component, sbs_log_level_t level);

/**
 * Set log level at runtime.
 */
void sbs_log_set_level(sbs_log_level_t level);

/**
 * Get the current log level.
 */
sbs_log_level_t sbs_log_get_level(void);

/**
 * Return true when high-frequency profiling logs are enabled.
 * Disabled by default; set SBS_PROFILE_LOGS=1 to enable.
 */
bool sbs_log_profile_enabled(void);

/**
 * Set file output. Pass NULL to disable file logging.
 */
void sbs_log_set_file(FILE *fp);

/**
 * Parse a log level string ("fatal", "error", "warn", "info", "debug", "trace").
 * Returns SBS_LOG_INFO on unrecognized input.
 */
sbs_log_level_t sbs_log_level_from_string(const char *str);

/**
 * Return the human-readable name for a log level.
 */
const char *sbs_log_level_name(sbs_log_level_t level);

/**
 * Core log function with structured fields.
 * Prefer using the macros below.
 */
void sbs_log(sbs_log_level_t level, const char *component,
             const char *resource_id, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* ── Convenience Macros ───────────────────────────────────────── */

#define SBS_LOG_FATAL(comp, rid, ...) sbs_log(SBS_LOG_FATAL, comp, rid, __VA_ARGS__)
#define SBS_LOG_ERROR(comp, rid, ...) sbs_log(SBS_LOG_ERROR, comp, rid, __VA_ARGS__)
#define SBS_LOG_WARN(comp, rid, ...)  sbs_log(SBS_LOG_WARN,  comp, rid, __VA_ARGS__)
#define SBS_LOG_INFO(comp, rid, ...)  sbs_log(SBS_LOG_INFO,  comp, rid, __VA_ARGS__)
#define SBS_LOG_DEBUG(comp, rid, ...) sbs_log(SBS_LOG_DEBUG, comp, rid, __VA_ARGS__)
#define SBS_LOG_TRACE(comp, rid, ...) sbs_log(SBS_LOG_TRACE, comp, rid, __VA_ARGS__)

/* Component-specific shorthand (define SBS_LOG_COMP in each .c file) */
#define LOG_F(...) SBS_LOG_FATAL(SBS_LOG_COMP, NULL, __VA_ARGS__)
#define LOG_E(...) SBS_LOG_ERROR(SBS_LOG_COMP, NULL, __VA_ARGS__)
#define LOG_W(...) SBS_LOG_WARN(SBS_LOG_COMP, NULL, __VA_ARGS__)
#define LOG_I(...) SBS_LOG_INFO(SBS_LOG_COMP, NULL, __VA_ARGS__)
#define LOG_D(...) SBS_LOG_DEBUG(SBS_LOG_COMP, NULL, __VA_ARGS__)
#define LOG_T(...) SBS_LOG_TRACE(SBS_LOG_COMP, NULL, __VA_ARGS__)

#endif /* SBS_LOG_H */
