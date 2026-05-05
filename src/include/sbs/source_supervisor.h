/*
 * SBS - StreamBox Broadcast System
 * Source Supervisor — manages source worker processes
 *
 * Handles fork/exec of sbs-worker --mode=source, IPC frame reception,
 * crash detection with exponential backoff recovery, and graceful shutdown.
 *
 * Reference: document/05-source-manager.md sections 3-7
 */
#ifndef SBS_SOURCE_SUPERVISOR_H
#define SBS_SOURCE_SUPERVISOR_H

#include "sbs/types.h"
#include "sbs/frame_slot.h"
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Opaque Types ─────────────────────────────────────────────── */

typedef struct sbs_source_supervisor sbs_source_supervisor_t;
typedef struct sbs_scene_graph sbs_scene_graph_t;
typedef struct sbs_api_server sbs_api_server_t;

/* ── Source Configuration ─────────────────────────────────────── */

/**
 * Configuration for starting a source worker.
 *
 * The supervisor serializes this to JSON and pipes it to the worker's stdin.
 */
typedef struct sbs_source_start_config {
    const char *source_id;       /* Unique source ID */
    const char *source_type;     /* "videotestsrc", "streamboxsrc", "v4l2src", "image", "uridecodebin", "text" */
    uint32_t    width;
    uint32_t    height;
    uint32_t    framerate_num;
    uint32_t    framerate_den;

    /* Source-type-specific fields (optional, may be NULL/0) */
    const char *pattern;         /* videotestsrc pattern name */
    const char *capture_mode;    /* streamboxsrc capture mode */
    const char *output_format;   /* streamboxsrc output format */
    const char *device_path;     /* v4l2src device path */
    const char *uri;             /* image/media URI or path */
    bool        loop;            /* loop media on EOS */
    const char *text;            /* text source content */
    const char *font_family;     /* text source font family */
    const char *font_path;       /* uploaded/custom font file */
    const char *text_color;      /* #RRGGBB or #RRGGBBAA */
    const char *text_align;      /* left, center, right */
    uint32_t    font_size;       /* text source font size in pixels */
} sbs_source_start_config_t;

typedef struct sbs_source_supervisor_metrics {
    uint32_t total_sources;
    uint32_t connected_sources;
    uint32_t restarting_sources;
    uint32_t stale_sources;
    uint32_t max_restart_attempts;
    bool degraded;
} sbs_source_supervisor_metrics_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Create a new source supervisor.
 *
 * @param worker_path  Path to sbs-worker binary (e.g. "/usr/bin/sbs-worker")
 * @param sock_dir     Directory for IPC sockets (e.g. "/run/sbs")
 * @return New supervisor, or NULL on allocation failure
 */
sbs_source_supervisor_t *sbs_source_supervisor_new(const char *worker_path,
                                                    const char *sock_dir);

/**
 * Free the supervisor and all owned resources.
 *
 * If any workers are still running, they are killed (SIGKILL).
 * All sockets are closed and unlinked.
 */
void sbs_source_supervisor_free(sbs_source_supervisor_t *sup);

/**
 * Start a source worker.
 *
 * Creates a Unix socket at {sock_dir}/source-{id}.sock, fork/execs
 * sbs-worker --mode=source, pipes JSON config to stdin, accepts the
 * connection, and installs GLib I/O watch for frame reception.
 *
 * Received frames update the frame slot via atomic pointer swap.
 *
 * @param sup     Supervisor instance
 * @param config  Source start configuration
 * @param slot    Frame slot for this source (caller owns; must outlive source)
 * @return SBS_OK on success, negative error code on failure
 */
int sbs_source_supervisor_start_source(sbs_source_supervisor_t *sup,
                                        const sbs_source_start_config_t *config,
                                        sbs_frame_slot_t *slot);

/**
 * Stop a source worker by ID.
 *
 * Sends SBS_MSG_SHUTDOWN, waits for clean exit, escalates to SIGKILL if needed.
 *
 * @param sup        Supervisor instance
 * @param source_id  Source ID to stop
 * @return SBS_OK on success, SBS_ERR_NOT_FOUND if source not found
 */
int sbs_source_supervisor_stop_source(sbs_source_supervisor_t *sup,
                                       const char *source_id);

int sbs_source_supervisor_mute_source(sbs_source_supervisor_t *sup,
                                       const char *source_id);

int sbs_source_supervisor_unmute_source(sbs_source_supervisor_t *sup,
                                         const char *source_id);

/**
 * Shutdown all source workers.
 *
 * Implements the 4-stage shutdown sequence from doc 05 section 7:
 * Stage 0: Send SBS_MSG_SHUTDOWN to each worker
 * Stage 1: Wait up to 2s for clean exit
 * Stage 2: SIGTERM remaining workers
 * Stage 3: SIGKILL after additional 1s
 *
 * Blocks until all workers have exited.
 *
 * @param sup  Supervisor instance
 */
void sbs_source_supervisor_shutdown_all(sbs_source_supervisor_t *sup);

/**
 * Get number of active sources.
 */
uint32_t sbs_source_supervisor_source_count(const sbs_source_supervisor_t *sup);

void sbs_source_supervisor_get_metrics(const sbs_source_supervisor_t *sup,
                                       sbs_source_supervisor_metrics_t *metrics);

void sbs_source_supervisor_set_scene_graph(sbs_source_supervisor_t *sup,
                                           sbs_scene_graph_t *graph);
void sbs_source_supervisor_set_api_server(sbs_source_supervisor_t *sup,
                                          sbs_api_server_t *server);

#endif /* SBS_SOURCE_SUPERVISOR_H */
