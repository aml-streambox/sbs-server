/*
 * SBS - StreamBox Broadcast System
 * Source worker — GStreamer capture pipeline entry point
 */
#ifndef SBS_SOURCE_WORKER_H
#define SBS_SOURCE_WORKER_H

#include "worker_common.h"

/**
 * Run the source worker.
 *
 * Builds a GStreamer pipeline based on config.source.source_type:
 *   - "videotestsrc": test pattern generator (CPU buffers)
 *   - "streamboxsrc": HDMI capture (DMA-BUF)
 *   - "v4l2src": USB camera (DMA-BUF or mmap)
 *
 * Extracts frames from appsink and sends them to the supervisor via IPC.
 * Runs the GLib main loop until shutdown or pipeline error.
 *
 * @param ctx  Initialized worker context
 * @return SBS_OK on clean exit, negative error code on failure
 */
int source_worker_run(sbs_worker_ctx_t *ctx);

#endif /* SBS_SOURCE_WORKER_H */
