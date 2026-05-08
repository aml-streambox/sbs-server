#ifndef SBS_COMPOSITOR_THREAD_H
#define SBS_COMPOSITOR_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sbs/compositor.h"
#include "sbs/compositor_scene.h"
#include "sbs/export_dest.h"
#include "sbs/ipc.h"

typedef struct sbs_compositor_thread sbs_compositor_thread_t;

typedef struct {
    uint64_t frame_number;
    uint32_t frames_dropped;
} sbs_comp_event_t;

typedef struct sbs_native_canvas_lease {
    uint32_t entry_idx;
    uint64_t frame_number;
    uint64_t content_frame_number;
    int      backing_fd;
    size_t   backing_size;
    void    *host_mapped;
    size_t   host_size;
    bool     host_is_backing;
    uint32_t width;
    uint32_t height;
    uint32_t drm_format;
    uint64_t drm_modifier;
    uint32_t n_planes;
    uint32_t plane_offset[2];
    uint32_t plane_stride[2];
    sbs_export_color_mode_t color_mode;
} sbs_native_canvas_lease_t;

typedef struct sbs_native_canvas_mailbox_stats {
    bool     has_entry;
    uint32_t entry_idx;
    uint64_t frame_number;
    uint64_t published;
    uint64_t dropped;
    uint64_t consumed;
    uint64_t late;
} sbs_native_canvas_mailbox_stats_t;

sbs_compositor_thread_t *sbs_compositor_thread_new(uint32_t width,
                                                     uint32_t height,
                                                     uint32_t fps,
                                                     sbs_export_color_mode_t color_mode,
                                                     const char *shader_dir);
void  sbs_compositor_thread_free(sbs_compositor_thread_t *ct);

int   sbs_compositor_thread_start(sbs_compositor_thread_t *ct);
int   sbs_compositor_thread_stop(sbs_compositor_thread_t *ct);
int   sbs_compositor_thread_get_eventfd(sbs_compositor_thread_t *ct);
bool  sbs_compositor_thread_pop_event(sbs_compositor_thread_t *ct,
                                      sbs_comp_event_t **ev);
void  sbs_compositor_thread_drain_events(sbs_compositor_thread_t *ct);
int   sbs_compositor_thread_set_scene(sbs_compositor_thread_t *ct,
                                      const sbs_comp_scene_state_t *state);
void  sbs_compositor_thread_get_stats(sbs_compositor_thread_t *ct,
                                      uint64_t *frame_count,
                                      uint32_t *frames_dropped);

typedef struct {
    uint64_t frame_count;
    uint64_t content_frame_count;
    uint64_t repeated_frame_count;
    uint32_t frames_dropped;
    double   last_frame_time_ms;
    double   last_frame_latency_ms;
    double   last_frame_interval_ms;
    double   avg_frame_time_ms;
    double   min_frame_time_ms;
    double   max_frame_time_ms;
    double   fps_actual;
} sbs_comp_timing_stats_t;

void  sbs_compositor_thread_get_timing(sbs_compositor_thread_t *ct,
                                       sbs_comp_timing_stats_t *out);

/**
 * Export the most recently rendered target's DMA-BUF fd.
 *
 * This calls sbs_compositor_export_target_fd() on the target that was
 * most recently rendered (the one *before* current_target). The returned
 * fd is a dup'd copy — the caller owns it and must close it.
 *
 * IMPORTANT: Only call from the main thread's eventfd callback, after
 * draining events, when the compositor thread has moved on to the next
 * target and the rendered target's fence is guaranteed to have been
 * submitted.
 *
 * @param ct   Compositor thread instance
 * @param fd   Output: DMA-BUF fd of the last rendered frame
 * @return 0 on success, -1 on failure (fd set to -1)
 */
int   sbs_compositor_thread_export_frame(sbs_compositor_thread_t *ct,
                                         sbs_video_frame_msg_t *msg,
                                         int *fd);
int   sbs_compositor_thread_export_frame_ptr(sbs_compositor_thread_t *ct,
                                              sbs_video_frame_msg_t *msg,
                                              const void **data_out,
                                              size_t *size_out,
                                              uint32_t *target_idx_out);
int   sbs_compositor_thread_export_preview_frame(sbs_compositor_thread_t *ct,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 sbs_video_frame_msg_t *msg,
                                                 int *fd);
int   sbs_compositor_thread_export_preview_frame_ptr(sbs_compositor_thread_t *ct,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      sbs_video_frame_msg_t *msg,
                                                      const void **data_out,
                                                      size_t *size_out);
bool  sbs_compositor_thread_can_export_dmabuf(sbs_compositor_thread_t *ct);

/* ── Destination-oriented export API ──────────────────────────── */

/**
 * Initialize a destination export path via the compositor thread.
 */
int   sbs_compositor_thread_init_export_dest(sbs_compositor_thread_t *ct,
                                              sbs_export_dest_type_t type,
                                              uint32_t width, uint32_t height,
                                              sbs_export_color_mode_t color_mode);

/**
 * Submit an export pass for a destination, using the last rendered target.
 */
int   sbs_compositor_thread_export_dest_submit(sbs_compositor_thread_t *ct,
                                                sbs_export_dest_type_t type,
                                                uint64_t frame_number);

/**
 * Acquire the next ready frame from a destination.
 */
sbs_export_slot_t *sbs_compositor_thread_export_dest_acquire(sbs_compositor_thread_t *ct,
                                                              sbs_export_dest_type_t type);

/**
 * Release a slot back after consumption.
 */
void  sbs_compositor_thread_export_dest_release(sbs_compositor_thread_t *ct,
                                                 sbs_export_dest_type_t type,
                                                 sbs_export_slot_t *slot);

/**
 * Check if destination export is available.
 */
bool  sbs_compositor_thread_dest_export_available(sbs_compositor_thread_t *ct,
                                                    sbs_export_dest_type_t type);

bool  sbs_compositor_thread_native_canvas_enabled(sbs_compositor_thread_t *ct);
int   sbs_compositor_thread_configure_native_preview(sbs_compositor_thread_t *ct,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t frame_interval);
bool  sbs_compositor_thread_native_mailbox_acquire(sbs_compositor_thread_t *ct,
                                                     sbs_native_canvas_mailbox_type_t type,
                                                     sbs_native_canvas_lease_t *lease);
void  sbs_compositor_thread_native_mailbox_release(sbs_compositor_thread_t *ct,
                                                    const sbs_native_canvas_lease_t *lease);
void  sbs_compositor_thread_native_mailbox_get_stats(sbs_compositor_thread_t *ct,
                                                      sbs_native_canvas_mailbox_type_t type,
                                                      sbs_native_canvas_mailbox_stats_t *out);

void  sbs_compositor_thread_get_dimensions(sbs_compositor_thread_t *ct,
                                              uint32_t *width, uint32_t *height);
void  sbs_compositor_thread_get_canvas_config(sbs_compositor_thread_t *ct,
                                              uint32_t *width,
                                              uint32_t *height,
                                              uint32_t *fps,
                                              sbs_export_color_mode_t *color_mode);

#endif
