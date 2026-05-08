/*
 * SBS - StreamBox Broadcast System
 * Output Router — distributes composed frames to output workers
 *
 * The output router uses a dedicated export thread to avoid blocking the
 * GLib main loop. When the compositor renders a new frame, the main thread's
 * eventfd callback drains events and signals the export thread, which then
 * performs the blocking GPU export (fence waits, queue submit, RGBA→NV21
 * conversion) and fans the result out to all consumers.
 *
 * Reference: document/06-output-manager.md section 6
 */
#ifndef SBS_OUTPUT_ROUTER_H
#define SBS_OUTPUT_ROUTER_H

#include "sbs/output_supervisor.h"
#include "sbs/compositor_thread.h"
#include "sbs/preview.h"
#include "sbs/snapshot.h"
#include "sbs/audio_mixer.h"
#include "sbs/encoder_manager.h"
#include "sbs/export_dest.h"
#include <glib.h>
#include <stdint.h>

/* ── Opaque Types ─────────────────────────────────────────────── */

typedef struct sbs_output_router sbs_output_router_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * Create a new output router.
 *
 * @param comp_thread  Compositor thread (for frame export)
 * @param out_sup      Output supervisor (for frame delivery to workers)
 * @param width        Canvas width (for frame metadata)
 * @param height       Canvas height
 * @param fps          Target framerate
 * @return New router, or NULL on allocation failure
 */
sbs_output_router_t *sbs_output_router_new(sbs_compositor_thread_t *comp_thread,
                                            sbs_output_supervisor_t *out_sup,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t fps);

/**
 * Free the output router.
 */
void sbs_output_router_free(sbs_output_router_t *router);

/**
 * Called from the main thread's eventfd callback when the compositor
 * renders a new frame. Exports the DMA-BUF fd and distributes it
 * to all active output workers.
 *
 * This function handles:
 * - Draining the eventfd and processing compositor events
 * - Exporting the latest rendered target's DMA-BUF fd
 * - Building a sbs_video_frame_msg_t with frame metadata
 * - Calling output_supervisor_send_frame() to fan out
 * - Closing the exported fd after distribution
 * - Logging frame statistics periodically
 *
 * @param router  Output router instance
 */
void sbs_output_router_distribute_frame(sbs_output_router_t *router);

/**
 * Get the GLib I/O callback for use with g_io_add_watch on the
 * compositor eventfd. This is a convenience — the callback internally
 * calls sbs_output_router_distribute_frame().
 *
 * Usage:
 *   int efd = sbs_compositor_thread_get_eventfd(comp_thread);
 *   GIOChannel *chan = g_io_channel_unix_new(efd);
 *   g_io_add_watch(chan, G_IO_IN,
 *                  sbs_output_router_on_frame_ready, router);
 *
 * @return GIOFunc compatible callback
 */
gboolean sbs_output_router_on_frame_ready(GIOChannel *source,
                                           GIOCondition condition,
                                           gpointer user_data);

gboolean sbs_output_router_on_frame_ready_fd(gint fd,
                                             GIOCondition condition,
                                             gpointer user_data);

/**
 * Get total frames distributed.
 */
uint64_t sbs_output_router_frames_distributed(const sbs_output_router_t *router);
double sbs_output_router_last_encoder_time_ms(const sbs_output_router_t *router);

void sbs_output_router_set_preview_engine(sbs_output_router_t *router,
                                          sbs_preview_engine_t *preview);
void sbs_output_router_set_snapshot_engine(sbs_output_router_t *router,
                                           sbs_snapshot_engine_t *snapshot);
void sbs_output_router_set_audio_mixer(sbs_output_router_t *router,
                                       sbs_audio_mixer_t *audio);
void sbs_output_router_set_encoder_manager(sbs_output_router_t *router,
                                            sbs_encoder_manager_t *encoder_mgr);
void sbs_output_router_set_color_mode(sbs_output_router_t *router,
                                       sbs_export_color_mode_t color_mode);

#endif /* SBS_OUTPUT_ROUTER_H */
