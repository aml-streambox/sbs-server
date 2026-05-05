/*
 * SBS - StreamBox Broadcast System
 * Output worker header
 */
#ifndef SBS_OUTPUT_WORKER_H
#define SBS_OUTPUT_WORKER_H

#include "worker_common.h"

/**
 * Run the output worker main loop.
 *
 * Builds a GStreamer pipeline (appsrc → encoder → muxer → sink),
 * receives composed frames from the supervisor via IPC, wraps DMA-BUF
 * fds into GstBuffers, and pushes them into the pipeline.
 *
 * @param ctx  Initialized worker context
 * @return SBS_OK on clean exit, negative error code on failure
 */
int output_worker_run(sbs_worker_ctx_t *ctx);

#endif /* SBS_OUTPUT_WORKER_H */
