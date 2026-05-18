/*
 * SBS - StreamBox Broadcast System
 * Output Router — fan-out composed frames to output workers
 *
 * Uses a dedicated export thread to perform blocking GPU exports (fence
 * waits, queue submits, RGBA→NV21 conversion) off the GLib main thread.
 * The main thread's eventfd callback only drains compositor events and
 * signals the export thread, returning immediately.
 *
 * Reference: document/06-output-manager.md section 6
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "out-router"

#include "sbs/output_router.h"
#include "sbs/encoder_manager.h"
#include "sbs/export_dest.h"
#include "sbs/ipc.h"
#include "sbs/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

/* DRM format codes (see drm_fourcc.h) */
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif
#ifndef DRM_FORMAT_NV21
#define DRM_FORMAT_NV21 0x3132564e
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif

/* ── Output Router ────────────────────────────────────────────── */

#define SBS_NATIVE_ENCODER_QUEUE_CAPACITY 2u
#define SBS_NATIVE_ENCODER_RETAIN_CAPACITY 16u
#define SBS_NATIVE_ENCODER_BFRAME_RETAIN_COUNT 8u
#define SBS_AUDIO_ROUTER_INTERVAL_US 2000u

struct sbs_output_router {
    sbs_compositor_thread_t  *comp_thread;  /* Borrowed reference */
    sbs_output_supervisor_t  *out_sup;      /* Borrowed reference */

    uint32_t  width;
    uint32_t  height;
    uint32_t  fps;
    sbs_preview_engine_t *preview;      /* Borrowed reference */
    sbs_snapshot_engine_t *snapshot;    /* Borrowed reference */
    sbs_audio_mixer_t *audio;           /* Borrowed reference */
    sbs_encoder_manager_t *encoder_mgr; /* Borrowed reference */

    sbs_export_color_mode_t color_mode;

    uint64_t  frame_count;           /* Total frames processed */
    uint64_t  frames_distributed;    /* Frames sent to at least one output */
    uint64_t  frames_skipped;        /* Frames skipped (no outputs or export failed) */
    uint64_t  sequence;              /* Monotonic frame sequence counter */

    /* Export thread */
    pthread_t  export_thread;
    int        export_eventfd;       /* Signaled by main thread to wake export thread */
    volatile bool export_running;
    bool       export_started;

    /* Audio routing is independent from compositor/export cadence. */
    pthread_t       audio_thread;
    bool            audio_thread_started;
    volatile bool   audio_thread_running;
    pthread_mutex_t audio_lock;
    pthread_mutex_t supervisor_send_lock;

    /* Destination-oriented export state */
    bool       dest_preview_inited;
    bool       dest_output_inited;
    uint32_t   dest_preview_width;
    uint32_t   dest_preview_height;

    bool       native_preview_configured;
    uint32_t   native_preview_width;
    uint32_t   native_preview_height;
    uint32_t   native_preview_frame_interval;
    sbs_export_color_mode_t native_preview_color_mode;

    /* Native output encoder handoff. 4K Wave521 encode can block for tens of
     * ms, so keep it off the router/export thread and retain the canvas lease
     * until the worker finishes reading it. */
    pthread_t       encoder_thread;
    bool            encoder_thread_started;
    bool            encoder_thread_running;
    pthread_mutex_t encoder_lock;
    pthread_cond_t  encoder_cond;
    uint32_t        encoder_queue_head;
    uint32_t        encoder_queue_count;
    sbs_native_canvas_lease_t encoder_queue_leases[SBS_NATIVE_ENCODER_QUEUE_CAPACITY];
    sbs_video_frame_msg_t     encoder_queue_msgs[SBS_NATIVE_ENCODER_QUEUE_CAPACITY];
    uint32_t        encoder_retain_head;
    uint32_t        encoder_retain_count;
    sbs_native_canvas_lease_t encoder_retained_leases[SBS_NATIVE_ENCODER_RETAIN_CAPACITY];
    uint64_t        encoder_frames_submitted;
    uint64_t        encoder_frames_dropped;
    uint64_t        encoder_last_content_frame;
    uint64_t        encoder_duplicate_frames_skipped;
    double          encoder_last_frame_time_ms;

    /* Native preview handoff. Preview's direct H.264 encode can also block, so
     * the router only publishes the latest profile-sized preview lease here. */
    pthread_t       preview_thread;
    bool            preview_thread_started;
    bool            preview_thread_running;
    pthread_mutex_t preview_lock;
    pthread_cond_t  preview_cond;
    bool            preview_has_pending;
    sbs_native_canvas_lease_t preview_pending_lease;
    sbs_video_frame_msg_t     preview_pending_msg;
    uint64_t        preview_frames_submitted;
    uint64_t        preview_frames_dropped;
};

/* ── Export Thread ────────────────────────────────────────────── */

static void native_lease_to_msg(const sbs_native_canvas_lease_t *lease,
                                uint64_t pts_ns,
                                uint64_t duration_ns,
                                uint64_t sequence,
                                sbs_video_frame_msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    sbs_video_frame_msg_init(msg, SBS_IPC_MSG_COMPOSED_FRAME);
    msg->width = lease->width;
    msg->height = lease->height;
    msg->drm_format = lease->drm_format;
    msg->drm_modifier = lease->drm_modifier;
    msg->n_planes = lease->n_planes;
    msg->plane_offset[0] = lease->plane_offset[0];
    msg->plane_offset[1] = lease->plane_offset[1];
    msg->plane_stride[0] = lease->plane_stride[0];
    msg->plane_stride[1] = lease->plane_stride[1];
    msg->buffer_type = SBS_FRAME_BUFFER_DMABUF;
    msg->pts_ns = pts_ns;
    msg->dts_ns = UINT64_MAX;
    msg->duration_ns = duration_ns;
    msg->sequence = sequence;
}

static void native_encoder_clear_lease(sbs_native_canvas_lease_t *lease)
{
    if (!lease)
        return;
    memset(lease, 0, sizeof(*lease));
    lease->backing_fd = -1;
}

static bool native_encoder_lease_valid(const sbs_native_canvas_lease_t *lease)
{
    return lease && lease->backing_fd >= 0 && lease->backing_size > 0;
}

static void native_encoder_release_lease(sbs_output_router_t *router,
                                          sbs_native_canvas_lease_t *lease)
{
    if (!router || !native_encoder_lease_valid(lease))
        return;
    sbs_compositor_thread_native_mailbox_release(router->comp_thread, lease);
    native_encoder_clear_lease(lease);
}

static uint32_t output_router_drain_audio(sbs_output_router_t *router)
{
    sbs_audio_mixer_t *audio;
    uint32_t drained = 0;

    if (!router)
        return 0;

    pthread_mutex_lock(&router->audio_lock);
    audio = router->audio;
    pthread_mutex_unlock(&router->audio_lock);
    if (!audio)
        return 0;

    for (;;) {
        sbs_audio_buffer_t *abuf = sbs_audio_mixer_take_latest_buffer(audio);
        sbs_encoder_manager_t *encoder_mgr = NULL;
        sbs_preview_engine_t *preview = NULL;
        if (!abuf)
            break;

        pthread_mutex_lock(&router->encoder_lock);
        encoder_mgr = router->encoder_mgr;
        pthread_mutex_unlock(&router->encoder_lock);
        if (encoder_mgr)
            sbs_encoder_manager_consume_audio(encoder_mgr, &abuf->msg, abuf->data);

        pthread_mutex_lock(&router->supervisor_send_lock);
        if (sbs_output_supervisor_output_count(router->out_sup) > 0)
            sbs_output_supervisor_send_audio(router->out_sup, &abuf->msg, abuf->data);
        pthread_mutex_unlock(&router->supervisor_send_lock);

        pthread_mutex_lock(&router->preview_lock);
        preview = router->preview;
        pthread_mutex_unlock(&router->preview_lock);
        if (preview)
            sbs_preview_engine_consume_audio(preview, &abuf->msg, abuf->data);

        g_free(abuf);
        drained++;
    }

    return drained;
}

static void *audio_thread_func(void *arg)
{
    sbs_output_router_t *router = arg;

    LOG_I("audio routing thread started");
    while (router->audio_thread_running) {
        output_router_drain_audio(router);
        usleep(SBS_AUDIO_ROUTER_INTERVAL_US);
    }
    output_router_drain_audio(router);
    LOG_I("audio routing thread stopped");
    return NULL;
}

static bool native_encoder_content_already_queued(sbs_output_router_t *router,
                                                  uint64_t content_frame_number)
{
    bool duplicate = false;

    if (!router || content_frame_number == 0)
        return false;

    pthread_mutex_lock(&router->encoder_lock);
    duplicate = router->encoder_last_content_frame == content_frame_number;
    pthread_mutex_unlock(&router->encoder_lock);

    return duplicate;
}

static void native_encoder_note_content_queued(sbs_output_router_t *router,
                                               uint64_t content_frame_number)
{
    if (!router || content_frame_number == 0)
        return;

    pthread_mutex_lock(&router->encoder_lock);
    router->encoder_last_content_frame = content_frame_number;
    pthread_mutex_unlock(&router->encoder_lock);
}

static uint64_t native_encoder_note_duplicate_skipped(sbs_output_router_t *router)
{
    uint64_t skipped = 0;

    if (!router)
        return 0;

    pthread_mutex_lock(&router->encoder_lock);
    skipped = ++router->encoder_duplicate_frames_skipped;
    pthread_mutex_unlock(&router->encoder_lock);

    return skipped;
}

static bool native_encoder_encode_duplicates_enabled(void)
{
    const char *env = getenv("SBS_NATIVE_ENCODER_ENCODE_DUPLICATES");
    return !env || env[0] != '0';
}

static bool native_encoder_bframes_enabled(sbs_encoder_manager_t *encoder_mgr)
{
    sbs_encoder_config_t cfg = {0};

    if (!encoder_mgr)
        return false;

    sbs_encoder_manager_get_config(encoder_mgr, &cfg);
    switch (cfg.gop_pattern) {
    case 1:
    case 2:
    case 3:
    case 6:
    case 7:
        return true;
    default:
        return false;
    }
}

static void native_encoder_retain_lease(sbs_output_router_t *router,
                                        sbs_native_canvas_lease_t *lease)
{
    if (!router || !native_encoder_lease_valid(lease))
        return;

    if (router->encoder_retain_count >= SBS_NATIVE_ENCODER_RETAIN_CAPACITY) {
        native_encoder_release_lease(router,
            &router->encoder_retained_leases[router->encoder_retain_head]);
        router->encoder_retain_head = (router->encoder_retain_head + 1u) %
                                      SBS_NATIVE_ENCODER_RETAIN_CAPACITY;
        router->encoder_retain_count--;
    }

    uint32_t tail = (router->encoder_retain_head + router->encoder_retain_count) %
                    SBS_NATIVE_ENCODER_RETAIN_CAPACITY;
    router->encoder_retained_leases[tail] = *lease;
    router->encoder_retain_count++;
    native_encoder_clear_lease(lease);

    while (router->encoder_retain_count > SBS_NATIVE_ENCODER_BFRAME_RETAIN_COUNT) {
        native_encoder_release_lease(router,
            &router->encoder_retained_leases[router->encoder_retain_head]);
        router->encoder_retain_head = (router->encoder_retain_head + 1u) %
                                      SBS_NATIVE_ENCODER_RETAIN_CAPACITY;
        router->encoder_retain_count--;
    }
}

static void native_encoder_release_retained_leases(sbs_output_router_t *router)
{
    if (!router)
        return;

    while (router->encoder_retain_count > 0) {
        native_encoder_release_lease(router,
            &router->encoder_retained_leases[router->encoder_retain_head]);
        router->encoder_retain_head = (router->encoder_retain_head + 1u) %
                                      SBS_NATIVE_ENCODER_RETAIN_CAPACITY;
        router->encoder_retain_count--;
    }
    router->encoder_retain_head = 0;
}

static void native_encoder_reset_content_gate(sbs_output_router_t *router)
{
    if (!router)
        return;

    pthread_mutex_lock(&router->encoder_lock);
    router->encoder_last_content_frame = 0;
    pthread_mutex_unlock(&router->encoder_lock);
}

static void native_encoder_note_frame_time(sbs_output_router_t *router,
                                           gint64 elapsed_usec)
{
    if (!router)
        return;

    pthread_mutex_lock(&router->encoder_lock);
    router->encoder_last_frame_time_ms = (double)elapsed_usec / 1000.0;
    pthread_mutex_unlock(&router->encoder_lock);
}

static bool native_encoder_enqueue_lease(sbs_output_router_t *router,
                                         const sbs_native_canvas_lease_t *lease,
                                         const sbs_video_frame_msg_t *msg)
{
    sbs_native_canvas_lease_t dropped;
    native_encoder_clear_lease(&dropped);

    if (!router || !native_encoder_lease_valid(lease) || !msg)
        return false;

    pthread_mutex_lock(&router->encoder_lock);
    if (!router->encoder_mgr || !router->encoder_thread_running) {
        pthread_mutex_unlock(&router->encoder_lock);
        return false;
    }

    if (router->encoder_queue_count >= SBS_NATIVE_ENCODER_QUEUE_CAPACITY) {
        dropped = router->encoder_queue_leases[router->encoder_queue_head];
        native_encoder_clear_lease(&router->encoder_queue_leases[router->encoder_queue_head]);
        memset(&router->encoder_queue_msgs[router->encoder_queue_head], 0,
               sizeof(router->encoder_queue_msgs[router->encoder_queue_head]));
        router->encoder_queue_head = (router->encoder_queue_head + 1u) %
                                     SBS_NATIVE_ENCODER_QUEUE_CAPACITY;
        router->encoder_queue_count--;
        router->encoder_frames_dropped++;
    }

    uint32_t tail = (router->encoder_queue_head + router->encoder_queue_count) %
                    SBS_NATIVE_ENCODER_QUEUE_CAPACITY;
    router->encoder_queue_leases[tail] = *lease;
    router->encoder_queue_msgs[tail] = *msg;
    router->encoder_queue_count++;
    pthread_cond_signal(&router->encoder_cond);
    pthread_mutex_unlock(&router->encoder_lock);

    native_encoder_release_lease(router, &dropped);
    return true;
}

static void *native_encoder_thread_func(void *arg)
{
    sbs_output_router_t *router = arg;

    LOG_I("native encoder handoff thread started");

    for (;;) {
        sbs_native_canvas_lease_t lease;
        sbs_video_frame_msg_t msg;
        sbs_encoder_manager_t *encoder_mgr;

        native_encoder_clear_lease(&lease);
        memset(&msg, 0, sizeof(msg));

        pthread_mutex_lock(&router->encoder_lock);
        while (router->encoder_thread_running && router->encoder_queue_count == 0) {
            pthread_cond_wait(&router->encoder_cond, &router->encoder_lock);
        }
        if (!router->encoder_thread_running && router->encoder_queue_count == 0) {
            pthread_mutex_unlock(&router->encoder_lock);
            break;
        }

        lease = router->encoder_queue_leases[router->encoder_queue_head];
        msg = router->encoder_queue_msgs[router->encoder_queue_head];
        encoder_mgr = router->encoder_mgr;
        native_encoder_clear_lease(&router->encoder_queue_leases[router->encoder_queue_head]);
        memset(&router->encoder_queue_msgs[router->encoder_queue_head], 0,
               sizeof(router->encoder_queue_msgs[router->encoder_queue_head]));
        router->encoder_queue_head = (router->encoder_queue_head + 1u) %
                                     SBS_NATIVE_ENCODER_QUEUE_CAPACITY;
        router->encoder_queue_count--;
        pthread_cond_signal(&router->encoder_cond);
        pthread_mutex_unlock(&router->encoder_lock);

        if (!encoder_mgr || !native_encoder_lease_valid(&lease)) {
            native_encoder_release_lease(router, &lease);
            continue;
        }

        gint64 t0 = g_get_monotonic_time();
        bool submitted = false;
        int enc_fd = dup(lease.backing_fd);
        if (enc_fd >= 0) {
            sbs_encoder_manager_consume_frame_dmabuf(encoder_mgr, &msg,
                                                     enc_fd, lease.backing_size);
            submitted = true;
        } else {
            LOG_W("native encoder dup(fd=%d) failed: %s",
                  lease.backing_fd, strerror(errno));
        }

        gint64 elapsed = g_get_monotonic_time() - t0;
        uint64_t submitted_total;
        uint64_t dropped_total;
        double elapsed_ms = (double)elapsed / 1000.0;
        pthread_mutex_lock(&router->encoder_lock);
        if (submitted) {
            router->encoder_frames_submitted++;
            router->encoder_last_frame_time_ms = elapsed_ms;
        }
        submitted_total = router->encoder_frames_submitted;
        dropped_total = router->encoder_frames_dropped;
        pthread_mutex_unlock(&router->encoder_lock);

        if (submitted && (elapsed > 30000 || submitted_total % 60 == 0)) {
            LOG_I("NATIVE ENCODER frame=%lu total=%.1fms submitted=%lu dropped=%lu src=dmabuf",
                  (unsigned long)lease.frame_number,
                  elapsed / 1000.0,
                  (unsigned long)submitted_total,
                  (unsigned long)dropped_total);
        }

        if (submitted && native_encoder_bframes_enabled(encoder_mgr)) {
            native_encoder_retain_lease(router, &lease);
        } else {
            native_encoder_release_retained_leases(router);
            native_encoder_release_lease(router, &lease);
        }
    }

    native_encoder_release_retained_leases(router);

    LOG_I("native encoder handoff thread stopped");
    return NULL;
}

static bool native_preview_enqueue_lease(sbs_output_router_t *router,
                                         const sbs_native_canvas_lease_t *lease,
                                         const sbs_video_frame_msg_t *msg)
{
    sbs_native_canvas_lease_t dropped;
    native_encoder_clear_lease(&dropped);

    if (!router || !native_encoder_lease_valid(lease) || !msg)
        return false;

    pthread_mutex_lock(&router->preview_lock);
    if (!router->preview || !router->preview_thread_running) {
        pthread_mutex_unlock(&router->preview_lock);
        return false;
    }

    if (router->preview_has_pending) {
        dropped = router->preview_pending_lease;
        native_encoder_clear_lease(&router->preview_pending_lease);
        router->preview_has_pending = false;
        router->preview_frames_dropped++;
    }

    router->preview_pending_lease = *lease;
    router->preview_pending_msg = *msg;
    router->preview_has_pending = true;
    pthread_cond_signal(&router->preview_cond);
    pthread_mutex_unlock(&router->preview_lock);

    native_encoder_release_lease(router, &dropped);
    return true;
}

static void *native_preview_thread_func(void *arg)
{
    sbs_output_router_t *router = arg;

    LOG_I("native preview handoff thread started");

    for (;;) {
        sbs_native_canvas_lease_t lease;
        sbs_video_frame_msg_t msg;
        sbs_preview_engine_t *preview;

        native_encoder_clear_lease(&lease);
        memset(&msg, 0, sizeof(msg));

        pthread_mutex_lock(&router->preview_lock);
        while (router->preview_thread_running && !router->preview_has_pending) {
            pthread_cond_wait(&router->preview_cond, &router->preview_lock);
        }
        if (!router->preview_thread_running && !router->preview_has_pending) {
            pthread_mutex_unlock(&router->preview_lock);
            break;
        }

        lease = router->preview_pending_lease;
        msg = router->preview_pending_msg;
        preview = router->preview;
        native_encoder_clear_lease(&router->preview_pending_lease);
        memset(&router->preview_pending_msg, 0, sizeof(router->preview_pending_msg));
        router->preview_has_pending = false;
        pthread_mutex_unlock(&router->preview_lock);

        if (!preview || !native_encoder_lease_valid(&lease)) {
            native_encoder_release_lease(router, &lease);
            continue;
        }

        gint64 t0 = g_get_monotonic_time();
        bool submitted = false;
        int preview_fd = dup(lease.backing_fd);
        if (preview_fd >= 0) {
            sbs_preview_engine_consume_frame_dmabuf(preview, &msg,
                                                    preview_fd, lease.backing_size);
            submitted = true;
        } else {
            LOG_W("native preview dup(fd=%d) failed: %s",
                  lease.backing_fd, strerror(errno));
        }

        gint64 elapsed = g_get_monotonic_time() - t0;
        uint64_t submitted_total;
        uint64_t dropped_total;
        pthread_mutex_lock(&router->preview_lock);
        if (submitted)
            router->preview_frames_submitted++;
        submitted_total = router->preview_frames_submitted;
        dropped_total = router->preview_frames_dropped;
        pthread_mutex_unlock(&router->preview_lock);

        if (submitted && (elapsed > 20000 || submitted_total % 60 == 0)) {
            LOG_I("NATIVE PREVIEW frame=%lu push=%.1fms submitted=%lu dropped=%lu",
                  (unsigned long)lease.frame_number,
                  elapsed / 1000.0,
                  (unsigned long)submitted_total,
                  (unsigned long)dropped_total);
        }

        native_encoder_release_lease(router, &lease);
    }

    LOG_I("native preview handoff thread stopped");
    return NULL;
}

static void export_thread_process_frame(sbs_output_router_t *router,
                                        uint64_t latest_frame_number,
                                        uint32_t latest_dropped)
{
    gint64 t_start, t_output_export, t_preview_export, t_preview_consume,
           t_encoder_consume, t_total;

    if (latest_frame_number == 0) {
        return;
    }

    router->frame_count++;
    t_start = g_get_monotonic_time();
    t_output_export = 0;
    t_preview_export = 0;
    t_preview_consume = 0;
    t_encoder_consume = 0;

    /* Check if any outputs are active */
    uint32_t output_count = sbs_output_supervisor_output_count(router->out_sup);
    sbs_encoder_manager_t *encoder_mgr = NULL;
    pthread_mutex_lock(&router->encoder_lock);
    encoder_mgr = router->encoder_mgr;
    pthread_mutex_unlock(&router->encoder_lock);
    uint32_t encoder_sink_count = encoder_mgr
        ? sbs_encoder_manager_sink_count(encoder_mgr) : 0;
    if (encoder_sink_count == 0)
        native_encoder_reset_content_gate(router);
    bool snapshot_needs_frame = router->snapshot && sbs_snapshot_engine_needs_frame(router->snapshot);
    sbs_preview_engine_t *preview = NULL;
    pthread_mutex_lock(&router->preview_lock);
    preview = router->preview;
    pthread_mutex_unlock(&router->preview_lock);
    sbs_preview_profile_t *preview_profile = preview
        ? sbs_preview_engine_active_fallback(preview)
        : NULL;
    uint32_t preview_frame_interval = 1;
    sbs_export_color_mode_t preview_native_color_mode = SBS_EXPORT_COLOR_SDR;
    if (preview_profile && preview_profile->framerate > 0 &&
        router->fps > preview_profile->framerate) {
        preview_frame_interval = (router->fps + preview_profile->framerate - 1u) /
                                 preview_profile->framerate;
        if (preview_frame_interval == 0)
            preview_frame_interval = 1;
    }
    if (preview_profile &&
        preview_profile->active_color_mode == SBS_PREVIEW_COLOR_MODE_HDR10) {
        preview_native_color_mode = SBS_EXPORT_COLOR_HDR10;
    }
    if (!preview_profile && router->native_preview_configured) {
        if (sbs_compositor_thread_configure_native_preview(
                router->comp_thread, 0, 0, 1, SBS_EXPORT_COLOR_SDR) == 0) {
            router->native_preview_configured = false;
            router->native_preview_width = 0;
            router->native_preview_height = 0;
            router->native_preview_frame_interval = 0;
            router->native_preview_color_mode = SBS_EXPORT_COLOR_SDR;
            LOG_I("native preview GPU target release requested");
        }
    }
    if (output_count == 0 && encoder_sink_count == 0 && !snapshot_needs_frame && !preview_profile) {
        router->frames_skipped++;
        return;
    }

    bool preview_only = (output_count == 0 && encoder_sink_count == 0 && !snapshot_needs_frame && preview_profile);
    bool need_output = (output_count > 0 || encoder_sink_count > 0 || snapshot_needs_frame);
    bool need_preview = (preview_profile != NULL);
    bool native_canvas_active = sbs_compositor_thread_native_canvas_enabled(router->comp_thread);
    bool dest_preview_ok = false;
    bool dest_output_ok = false;
    uint32_t pacing_fps = (preview_only && preview_profile && preview_profile->framerate > 0)
        ? preview_profile->framerate
        : router->fps;
    router->sequence++;
    uint64_t duration_ns = (pacing_fps > 0)
                           ? (uint64_t)(1000000000ULL / pacing_fps)
                           : 16666667ULL;
    uint64_t pts_ns = (router->sequence - 1) * duration_ns;

    if (native_canvas_active) {
        bool delivered = false;
        bool need_program_output = (output_count > 0 || encoder_sink_count > 0);

        if (need_preview && preview_profile && preview_profile->width > 0 &&
            preview_profile->height > 0 &&
            (!router->native_preview_configured ||
             router->native_preview_width != preview_profile->width ||
             router->native_preview_height != preview_profile->height ||
             router->native_preview_frame_interval != preview_frame_interval ||
              router->native_preview_color_mode != preview_native_color_mode)) {
            if (sbs_compositor_thread_configure_native_preview(
                    router->comp_thread,
                    preview_profile->width,
                    preview_profile->height,
                    preview_frame_interval,
                    preview_native_color_mode) == 0) {
                router->native_preview_configured = true;
                router->native_preview_width = preview_profile->width;
                router->native_preview_height = preview_profile->height;
                router->native_preview_frame_interval = preview_frame_interval;
                router->native_preview_color_mode = preview_native_color_mode;
                LOG_I("native preview GPU target requested: %ux%u mode=%s interval=%u",
                      router->native_preview_width,
                      router->native_preview_height,
                      router->native_preview_color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr",
                      router->native_preview_frame_interval);
            }
        }

        if (need_program_output) {
            sbs_native_canvas_lease_t lease;
            if (sbs_compositor_thread_native_mailbox_acquire(
                    router->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_OUTPUT, &lease)) {
                bool encoder_lease_transferred = false;
                sbs_video_frame_msg_t msg;
                native_lease_to_msg(&lease, pts_ns, duration_ns, router->sequence, &msg);

                if (lease.color_mode == SBS_EXPORT_COLOR_SDR) {
                    if (output_count > 0) {
                        pthread_mutex_lock(&router->supervisor_send_lock);
                        int sent = sbs_output_supervisor_send_frame(router->out_sup,
                                                                    &msg,
                                                                    lease.backing_fd);
                        pthread_mutex_unlock(&router->supervisor_send_lock);
                        if (sent > 0) {
                            router->frames_distributed++;
                            delivered = true;
                        }
                    }
                } else if (output_count > 0) {
                    static uint32_t native_hdr_skip_warn_counter = 0;
                    if (native_hdr_skip_warn_counter++ % 300 == 0) {
                        LOG_I("native HDR10 output lease is P010; legacy supervisor consumers skipped");
                    }
                }

                if (encoder_sink_count > 0 && encoder_mgr) {
                    gint64 t0 = g_get_monotonic_time();
                    uint64_t content_frame_number = lease.content_frame_number != 0
                        ? lease.content_frame_number : lease.frame_number;
                    if (!native_encoder_encode_duplicates_enabled() &&
                        native_encoder_content_already_queued(router, content_frame_number)) {
                        uint64_t skipped = native_encoder_note_duplicate_skipped(router);
                        delivered = true;
                        if (skipped <= 5 || skipped % 60 == 0) {
                            LOG_I("NATIVE ENCODER duplicate skipped frame=%lu content=%lu skipped=%lu",
                                  (unsigned long)lease.frame_number,
                                  (unsigned long)content_frame_number,
                                  (unsigned long)skipped);
                        }
                    } else {
                        encoder_lease_transferred = native_encoder_enqueue_lease(router, &lease, &msg);
                    }
                    if (encoder_lease_transferred) {
                        native_encoder_note_content_queued(router, content_frame_number);
                        delivered = true;
                    } else {
                        if (!native_encoder_content_already_queued(router, content_frame_number)) {
                            LOG_W("native encoder handoff failed; dropping frame %lu",
                                  (unsigned long)lease.frame_number);
                        }
                    }
                    t_encoder_consume = g_get_monotonic_time() - t0;
                }

                if (!encoder_lease_transferred)
                    sbs_compositor_thread_native_mailbox_release(router->comp_thread, &lease);
            }
        }

        if (snapshot_needs_frame && router->snapshot) {
            sbs_native_canvas_lease_t lease;
            if (sbs_compositor_thread_native_mailbox_acquire(
                    router->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_SNAPSHOT, &lease)) {
                if (lease.color_mode == SBS_EXPORT_COLOR_SDR) {
                    sbs_video_frame_msg_t msg;
                    native_lease_to_msg(&lease, pts_ns, duration_ns,
                                        router->sequence, &msg);
                    int snap_fd = dup(lease.backing_fd);
                    if (snap_fd >= 0) {
                        sbs_snapshot_engine_consume_frame(router->snapshot, &msg, snap_fd);
                        delivered = true;
                    }
                } else {
                    sbs_video_frame_msg_t msg;
                    native_lease_to_msg(&lease, pts_ns, duration_ns,
                                        router->sequence, &msg);
                    int snap_fd = dup(lease.backing_fd);
                    if (snap_fd >= 0) {
                        sbs_snapshot_engine_consume_frame(router->snapshot, &msg, snap_fd);
                        delivered = true;
                    }
                }
                sbs_compositor_thread_native_mailbox_release(router->comp_thread, &lease);
            }
        }

        if (need_preview && preview && preview_profile) {
            sbs_native_canvas_lease_t lease;
            if (sbs_compositor_thread_native_mailbox_acquire(
                    router->comp_thread, SBS_NATIVE_CANVAS_MAILBOX_PREVIEW, &lease)) {
                bool preview_lease_transferred = false;
                sbs_video_frame_msg_t pmsg;
                native_lease_to_msg(&lease, pts_ns, duration_ns,
                                    router->sequence, &pmsg);
                if (preview_profile->framerate > 0)
                    pmsg.duration_ns = 1000000000ULL / preview_profile->framerate;
                gint64 t0 = g_get_monotonic_time();
                preview_lease_transferred = native_preview_enqueue_lease(router, &lease, &pmsg);
                if (preview_lease_transferred) {
                    delivered = true;
                } else {
                    LOG_W("native preview handoff failed; dropping frame %lu",
                          (unsigned long)lease.frame_number);
                }
                t_preview_consume = g_get_monotonic_time() - t0;
                if (!preview_lease_transferred)
                    sbs_compositor_thread_native_mailbox_release(router->comp_thread, &lease);
            }
        }

        if (!delivered)
            router->frames_skipped++;
        goto audio_only;
    }

    /* Check if destination export is available */
    bool use_dest_export = sbs_compositor_thread_dest_export_available(
        router->comp_thread, SBS_EXPORT_DEST_PREVIEW) ||
        sbs_compositor_thread_dest_export_available(
            router->comp_thread, SBS_EXPORT_DEST_OUTPUT);

    /* Try to use destination-oriented export path */
    if (use_dest_export || (!router->dest_preview_inited && need_preview) ||
        (!router->dest_output_inited && need_output)) {

        /* Initialize preview destination on demand */
        if (need_preview && !router->dest_preview_inited && preview_profile) {
            gint64 t0 = g_get_monotonic_time();
            int rc = sbs_compositor_thread_init_export_dest(
                router->comp_thread, SBS_EXPORT_DEST_PREVIEW,
                preview_profile->width, preview_profile->height,
                SBS_EXPORT_COLOR_SDR);
            if (rc == 0) {
                router->dest_preview_inited = true;
                router->dest_preview_width = preview_profile->width;
                router->dest_preview_height = preview_profile->height;
                LOG_I("dest preview export initialized: %ux%u (%.1fms)",
                      preview_profile->width, preview_profile->height,
                      (g_get_monotonic_time() - t0) / 1000.0);
            }
        }

        /* Initialize output destination on demand */
        if (need_output && !router->dest_output_inited) {
            gint64 t0 = g_get_monotonic_time();
            int rc = sbs_compositor_thread_init_export_dest(
                router->comp_thread, SBS_EXPORT_DEST_OUTPUT,
                router->width, router->height,
                router->color_mode);
            if (rc == 0) {
                router->dest_output_inited = true;
                LOG_I("dest output export initialized: %ux%u (%.1fms)",
                      router->width, router->height,
                      (g_get_monotonic_time() - t0) / 1000.0);
            }
        }
    }

    /* ── Destination-oriented export path ──────────────────────── */

    dest_preview_ok = router->dest_preview_inited &&
        sbs_compositor_thread_dest_export_available(router->comp_thread, SBS_EXPORT_DEST_PREVIEW);
    dest_output_ok = router->dest_output_inited &&
        sbs_compositor_thread_dest_export_available(router->comp_thread, SBS_EXPORT_DEST_OUTPUT);

    /* Step 1: Submit export passes for active destinations */
    if (dest_output_ok && need_output) {
        gint64 t0 = g_get_monotonic_time();
        sbs_compositor_thread_export_dest_submit(router->comp_thread,
                                                  SBS_EXPORT_DEST_OUTPUT,
                                                  latest_frame_number);
        t_output_export = g_get_monotonic_time() - t0;
    }

    if (dest_preview_ok && need_preview) {
        /* Apply preview skip logic */
        uint32_t skip = 1;
        if (preview_profile->framerate > 0 && router->fps > preview_profile->framerate)
            skip = router->fps / preview_profile->framerate;
        if (skip <= 1 || (router->frame_count % skip) == 0) {
            gint64 t0 = g_get_monotonic_time();
            sbs_compositor_thread_export_dest_submit(router->comp_thread,
                                                      SBS_EXPORT_DEST_PREVIEW,
                                                      latest_frame_number);
            t_preview_export = g_get_monotonic_time() - t0;
        }
    }

    /* Step 2: Acquire ready frames and feed to consumers */
    /* Try to acquire output frame */
    if (dest_output_ok && need_output) {
        sbs_export_slot_t *slot = sbs_compositor_thread_export_dest_acquire(
            router->comp_thread, SBS_EXPORT_DEST_OUTPUT);
        if (slot && (slot->nv21_dmabuf_fd >= 0 || slot->nv21_mapped)) {
            sbs_video_frame_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            sbs_video_frame_msg_init(&msg, SBS_IPC_MSG_COMPOSED_FRAME);
            msg.width = router->width;
            msg.height = router->height;
            bool is_p010 = (router->color_mode == SBS_EXPORT_COLOR_HDR10);
            msg.drm_format = is_p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV21;
            msg.plane_offset[0] = 0;
            if (is_p010) {
                /* P010 output is one contiguous backing buffer/fd: Y followed
                 * by interleaved UV. Keep plane[1] layout metadata populated
                 * for consumers that need the UV byte offset, but advertise a
                 * single plane/fd so downstream never expects fd[1]. */
                msg.n_planes = 1;
                msg.plane_offset[1] = router->width * router->height * 2;
                msg.plane_stride[0] = router->width * 2;
                msg.plane_stride[1] = router->width * 2;
            } else {
                msg.n_planes = 2;
                msg.plane_offset[1] = router->width * router->height;
                msg.plane_stride[0] = router->width;
                msg.plane_stride[1] = router->width;
            }
            msg.buffer_type = SBS_FRAME_BUFFER_DMABUF;
            msg.pts_ns = pts_ns;
            msg.dts_ns = UINT64_MAX;
            msg.duration_ns = duration_ns;
            msg.sequence = router->sequence;

            /* Feed to encoder */
            if (encoder_mgr) {
                gint64 t0 = g_get_monotonic_time();
                if (slot->nv21_dmabuf_fd >= 0) {
                    int enc_fd = dup(slot->nv21_dmabuf_fd);
                    if (enc_fd >= 0) {
                        sbs_encoder_manager_consume_frame_dmabuf(encoder_mgr, &msg,
                                                                 enc_fd, slot->nv21_size);
                    } else {
                        LOG_W("export encoder dup(fd=%d) failed: %s",
                              slot->nv21_dmabuf_fd, strerror(errno));
                    }
                }
                t_encoder_consume = g_get_monotonic_time() - t0;
                if (t_encoder_consume > 0)
                    native_encoder_note_frame_time(router, t_encoder_consume);
            }

            /* Feed to legacy output supervisor */
            if (output_count > 0 || snapshot_needs_frame) {
                sbs_video_frame_msg_t memfd_msg = msg;
                memfd_msg.buffer_type = SBS_FRAME_BUFFER_MEMFD;
                /* Need to create memfd for legacy paths */
                size_t fsize = slot->nv21_size;
                int mfd = memfd_create("sbs-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
                if (mfd >= 0 && ftruncate(mfd, fsize) == 0) {
                    void *dst = mmap(NULL, fsize, PROT_WRITE, MAP_SHARED, mfd, 0);
                    if (dst != MAP_FAILED) {
                        memcpy(dst, slot->nv21_mapped, fsize);
                        munmap(dst, fsize);
                        if (snapshot_needs_frame) {
                            int snap_fd = dup(mfd);
                            if (snap_fd >= 0)
                                sbs_snapshot_engine_consume_frame(router->snapshot, &memfd_msg, snap_fd);
                        }
                        if (output_count > 0) {
                            pthread_mutex_lock(&router->supervisor_send_lock);
                            int sent = sbs_output_supervisor_send_frame(router->out_sup, &memfd_msg, mfd);
                            pthread_mutex_unlock(&router->supervisor_send_lock);
                            if (sent > 0) router->frames_distributed++;
                        }
                        mfd = -1; /* consumed by send_frame */
                    }
                }
                if (mfd >= 0) close(mfd);
            }

            sbs_compositor_thread_export_dest_release(router->comp_thread,
                                                       SBS_EXPORT_DEST_OUTPUT, slot);
        }
    }

    /* Try to acquire preview frame */
    if (dest_preview_ok && need_preview && preview) {
        sbs_export_slot_t *slot = sbs_compositor_thread_export_dest_acquire(
            router->comp_thread, SBS_EXPORT_DEST_PREVIEW);
        if (slot && (slot->nv21_dmabuf_fd >= 0 || slot->nv21_mapped)) {
            sbs_video_frame_msg_t pmsg;
            memset(&pmsg, 0, sizeof(pmsg));
            sbs_video_frame_msg_init(&pmsg, SBS_IPC_MSG_COMPOSED_FRAME);
            pmsg.width = router->dest_preview_width;
            pmsg.height = router->dest_preview_height;
            pmsg.drm_format = DRM_FORMAT_NV21;
            pmsg.n_planes = 2;
            pmsg.plane_offset[0] = 0;
            pmsg.plane_offset[1] = router->dest_preview_width * router->dest_preview_height;
            pmsg.plane_stride[0] = router->dest_preview_width;
            pmsg.plane_stride[1] = router->dest_preview_width;
            pmsg.buffer_type = SBS_FRAME_BUFFER_DMABUF;
            pmsg.pts_ns = pts_ns;
            pmsg.dts_ns = UINT64_MAX;
            pmsg.duration_ns = duration_ns;
            pmsg.sequence = router->sequence;

            gint64 t0 = g_get_monotonic_time();
            if (slot->nv21_dmabuf_fd >= 0) {
                int dup_fd = dup(slot->nv21_dmabuf_fd);
                if (dup_fd >= 0) {
                    sbs_preview_engine_consume_frame_dmabuf(preview, &pmsg,
                                                            dup_fd, slot->nv21_size);
                }
            } else {
                LOG_W("preview export slot has no DMA-BUF fd; dropping frame to avoid VMALLOC encoder input");
            }
            t_preview_consume = g_get_monotonic_time() - t0;

            sbs_compositor_thread_export_dest_release(router->comp_thread,
                                                       SBS_EXPORT_DEST_PREVIEW, slot);
        }
    }

audio_only:
    (void)encoder_sink_count;

    if (native_canvas_active) {
        return;
    }

    /* ── Legacy fallback removed — dest-export is mandatory ───── */
    if (!dest_output_ok && !dest_preview_ok) {
        static uint32_t no_dest_warn_counter = 0;
        if (no_dest_warn_counter++ % 300 == 0) {
            LOG_E("FATAL: dest-export not available — no fallback paths. "
                  "Ensure GPU compute export pipeline is initialized.");
        }
        router->frames_skipped++;
        return;
    }

    t_total = g_get_monotonic_time() - t_start;

    /* Log detailed timing every 60 frames (~1s) or when total > 20ms */
    bool should_log = (latest_frame_number % 60 == 0) || (t_total > 20000);
    if (should_log) {
        LOG_I("TIMING frame=%lu total=%.1fms out_exp=%.1fms prev_exp=%.1fms "
              "prev_push=%.1fms enc_push=%.1fms prev=%s enc_sinks=%u dropped=%u "
              "dest=%s",
              (unsigned long)latest_frame_number,
              t_total / 1000.0,
              t_output_export / 1000.0,
              t_preview_export / 1000.0,
              t_preview_consume / 1000.0,
              t_encoder_consume / 1000.0,
              need_preview ? "yes" : "skip",
              encoder_sink_count,
              latest_dropped,
              (dest_output_ok || dest_preview_ok) ? "new" : "legacy");
    }
}

static void export_thread_do_frame(sbs_output_router_t *router)
{
    sbs_comp_event_t *ev;

    /* Consume the readiness signal, then process every queued compositor
     * event. The native mailbox is FIFO, so this catches up after scheduler
     * jitter instead of collapsing several published frames into one latest
     * frame and reporting mailbox drops. */
    sbs_compositor_thread_drain_events(router->comp_thread);
    while (sbs_compositor_thread_pop_event(router->comp_thread, &ev)) {
        uint64_t frame_number = ev->frame_number;
        uint32_t frames_dropped = ev->frames_dropped;
        free(ev);
        export_thread_process_frame(router, frame_number, frames_dropped);
    }
}

static void *export_thread_func(void *arg)
{
    sbs_output_router_t *router = arg;
    int comp_efd = sbs_compositor_thread_get_eventfd(router->comp_thread);

    LOG_I("export thread started (watching compositor eventfd=%d)", comp_efd);

    while (router->export_running) {
        /* Use a 20ms timeout so we periodically poll export fences even when
         * the compositor is blocked waiting for retire_slot. This prevents a
         * deadlock where all slots are SUBMITTED and no one polls them. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(comp_efd, &rfds);
        /* Also watch our stop eventfd */
        FD_SET(router->export_eventfd, &rfds);
        int maxfd = comp_efd > router->export_eventfd ? comp_efd : router->export_eventfd;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            LOG_E("export thread select failed: %s", strerror(errno));
            break;
        }

        /* Check stop signal */
        if (FD_ISSET(router->export_eventfd, &rfds)) {
            uint64_t val;
            (void)read(router->export_eventfd, &val, sizeof(val));
            if (!router->export_running)
                break;
        }

        /* Process compositor frame */
        if (FD_ISSET(comp_efd, &rfds)) {
            export_thread_do_frame(router);
        }

        /* Timeout path: poll export fences so slots can transition from
         * SUBMITTED to READY even when the compositor is stalled. */
        if (ret == 0) {
            static uint64_t timeout_count = 0;
            timeout_count++;
            if (router->dest_output_inited) {
                sbs_export_slot_t *slot = sbs_compositor_thread_export_dest_acquire(
                    router->comp_thread, SBS_EXPORT_DEST_OUTPUT);
                if (slot) {
                    /* No consumer on timeout path — just release to free the slot */
                    LOG_I("TIMEOUT acquired output slot fn=%lu", (unsigned long)slot->frame_number);
                    sbs_compositor_thread_export_dest_release(
                        router->comp_thread, SBS_EXPORT_DEST_OUTPUT, slot);
                } else if (timeout_count % 50 == 0) {
                    LOG_I("TIMEOUT no output slot ready (poll %lu)", (unsigned long)timeout_count);
                }
            }
            if (router->dest_preview_inited) {
                sbs_export_slot_t *slot = sbs_compositor_thread_export_dest_acquire(
                    router->comp_thread, SBS_EXPORT_DEST_PREVIEW);
                if (slot) {
                    sbs_compositor_thread_export_dest_release(
                        router->comp_thread, SBS_EXPORT_DEST_PREVIEW, slot);
                }
            }
        }
    }

    LOG_I("export thread stopped");
    return NULL;
}

/* ── Lifecycle ────────────────────────────────────────────────── */

sbs_output_router_t *sbs_output_router_new(sbs_compositor_thread_t *comp_thread,
                                            sbs_output_supervisor_t *out_sup,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t fps)
{
    if (!comp_thread || !out_sup) return NULL;

    sbs_output_router_t *router = calloc(1, sizeof(*router));
    if (!router) return NULL;

    router->comp_thread = comp_thread;
    router->out_sup     = out_sup;
    router->width       = width;
    router->height      = height;
    router->fps         = fps;
    router->export_eventfd = -1;
    for (uint32_t i = 0; i < SBS_NATIVE_ENCODER_QUEUE_CAPACITY; i++)
        native_encoder_clear_lease(&router->encoder_queue_leases[i]);
    for (uint32_t i = 0; i < SBS_NATIVE_ENCODER_RETAIN_CAPACITY; i++)
        native_encoder_clear_lease(&router->encoder_retained_leases[i]);
    pthread_mutex_init(&router->audio_lock, NULL);
    pthread_mutex_init(&router->supervisor_send_lock, NULL);
    pthread_mutex_init(&router->encoder_lock, NULL);
    pthread_cond_init(&router->encoder_cond, NULL);

    router->encoder_thread_running = true;
    int rc = pthread_create(&router->encoder_thread, NULL,
                            native_encoder_thread_func, router);
    if (rc != 0) {
        LOG_E("failed to create native encoder handoff thread: %s", strerror(rc));
        pthread_cond_destroy(&router->encoder_cond);
        pthread_mutex_destroy(&router->encoder_lock);
        pthread_mutex_destroy(&router->supervisor_send_lock);
        pthread_mutex_destroy(&router->audio_lock);
        free(router);
        return NULL;
    }
    router->encoder_thread_started = true;

    native_encoder_clear_lease(&router->preview_pending_lease);
    pthread_mutex_init(&router->preview_lock, NULL);
    pthread_cond_init(&router->preview_cond, NULL);

    router->preview_thread_running = true;
    rc = pthread_create(&router->preview_thread, NULL,
                        native_preview_thread_func, router);
    if (rc != 0) {
        LOG_E("failed to create native preview handoff thread: %s", strerror(rc));
        pthread_mutex_lock(&router->encoder_lock);
        router->encoder_thread_running = false;
        pthread_cond_signal(&router->encoder_cond);
        pthread_mutex_unlock(&router->encoder_lock);
        pthread_join(router->encoder_thread, NULL);
        pthread_cond_destroy(&router->preview_cond);
        pthread_mutex_destroy(&router->preview_lock);
        pthread_cond_destroy(&router->encoder_cond);
        pthread_mutex_destroy(&router->encoder_lock);
        pthread_mutex_destroy(&router->supervisor_send_lock);
        pthread_mutex_destroy(&router->audio_lock);
        free(router);
        return NULL;
    }
    router->preview_thread_started = true;

    router->audio_thread_running = true;
    rc = pthread_create(&router->audio_thread, NULL, audio_thread_func, router);
    if (rc != 0) {
        LOG_E("failed to create audio routing thread: %s", strerror(rc));
        router->audio_thread_running = false;
    } else {
        router->audio_thread_started = true;
    }

    /* Create export thread */
    router->export_eventfd = eventfd(0, EFD_CLOEXEC);
    if (router->export_eventfd < 0) {
        LOG_E("failed to create export eventfd: %s", strerror(errno));
        if (router->audio_thread_started) {
            router->audio_thread_running = false;
            pthread_join(router->audio_thread, NULL);
        }
        pthread_mutex_lock(&router->preview_lock);
        router->preview_thread_running = false;
        pthread_cond_signal(&router->preview_cond);
        pthread_mutex_unlock(&router->preview_lock);
        pthread_join(router->preview_thread, NULL);
        pthread_mutex_lock(&router->encoder_lock);
        router->encoder_thread_running = false;
        pthread_cond_signal(&router->encoder_cond);
        pthread_mutex_unlock(&router->encoder_lock);
        pthread_join(router->encoder_thread, NULL);
        pthread_cond_destroy(&router->preview_cond);
        pthread_mutex_destroy(&router->preview_lock);
        pthread_cond_destroy(&router->encoder_cond);
        pthread_mutex_destroy(&router->encoder_lock);
        pthread_mutex_destroy(&router->supervisor_send_lock);
        pthread_mutex_destroy(&router->audio_lock);
        free(router);
        return NULL;
    }

    router->export_running = true;
    rc = pthread_create(&router->export_thread, NULL, export_thread_func, router);
    if (rc != 0) {
        LOG_E("failed to create export thread: %s", strerror(rc));
        if (router->audio_thread_started) {
            router->audio_thread_running = false;
            pthread_join(router->audio_thread, NULL);
        }
        pthread_mutex_lock(&router->preview_lock);
        router->preview_thread_running = false;
        pthread_cond_signal(&router->preview_cond);
        pthread_mutex_unlock(&router->preview_lock);
        pthread_join(router->preview_thread, NULL);
        pthread_mutex_lock(&router->encoder_lock);
        router->encoder_thread_running = false;
        pthread_cond_signal(&router->encoder_cond);
        pthread_mutex_unlock(&router->encoder_lock);
        pthread_join(router->encoder_thread, NULL);
        close(router->export_eventfd);
        pthread_cond_destroy(&router->preview_cond);
        pthread_mutex_destroy(&router->preview_lock);
        pthread_cond_destroy(&router->encoder_cond);
        pthread_mutex_destroy(&router->encoder_lock);
        pthread_mutex_destroy(&router->supervisor_send_lock);
        pthread_mutex_destroy(&router->audio_lock);
        free(router);
        return NULL;
    }
    router->export_started = true;

    LOG_I("output router created (%ux%u @ %u fps) with export thread", width, height, fps);
    return router;
}

void sbs_output_router_free(sbs_output_router_t *router)
{
    if (!router) return;

    /* Stop export thread */
    if (router->export_started) {
        router->export_running = false;
        if (router->export_eventfd >= 0) {
            /* Wake the thread so it sees export_running=false */
            uint64_t val = 1;
            (void)write(router->export_eventfd, &val, sizeof(val));
        }
        pthread_join(router->export_thread, NULL);
        LOG_I("export thread joined");
    }
    if (router->export_eventfd >= 0) {
        close(router->export_eventfd);
    }

    if (router->audio_thread_started) {
        router->audio_thread_running = false;
        pthread_join(router->audio_thread, NULL);
        LOG_I("audio routing thread joined");
    }

    if (router->preview_thread_started) {
        sbs_native_canvas_lease_t pending;
        native_encoder_clear_lease(&pending);

        pthread_mutex_lock(&router->preview_lock);
        router->preview_thread_running = false;
        if (router->preview_has_pending) {
            pending = router->preview_pending_lease;
            native_encoder_clear_lease(&router->preview_pending_lease);
            memset(&router->preview_pending_msg, 0, sizeof(router->preview_pending_msg));
            router->preview_has_pending = false;
        }
        pthread_cond_signal(&router->preview_cond);
        pthread_mutex_unlock(&router->preview_lock);

        native_encoder_release_lease(router, &pending);
        pthread_join(router->preview_thread, NULL);
        LOG_I("native preview handoff thread joined");
    }
    pthread_cond_destroy(&router->preview_cond);
    pthread_mutex_destroy(&router->preview_lock);

    if (router->encoder_thread_started) {
        sbs_native_canvas_lease_t pending[SBS_NATIVE_ENCODER_QUEUE_CAPACITY];
        uint32_t pending_count = 0;
        for (uint32_t i = 0; i < SBS_NATIVE_ENCODER_QUEUE_CAPACITY; i++)
            native_encoder_clear_lease(&pending[i]);

        pthread_mutex_lock(&router->encoder_lock);
        router->encoder_thread_running = false;
        while (router->encoder_queue_count > 0 &&
               pending_count < SBS_NATIVE_ENCODER_QUEUE_CAPACITY) {
            pending[pending_count++] = router->encoder_queue_leases[router->encoder_queue_head];
            native_encoder_clear_lease(&router->encoder_queue_leases[router->encoder_queue_head]);
            memset(&router->encoder_queue_msgs[router->encoder_queue_head], 0,
                   sizeof(router->encoder_queue_msgs[router->encoder_queue_head]));
            router->encoder_queue_head = (router->encoder_queue_head + 1u) %
                                         SBS_NATIVE_ENCODER_QUEUE_CAPACITY;
            router->encoder_queue_count--;
        }
        pthread_cond_signal(&router->encoder_cond);
        pthread_mutex_unlock(&router->encoder_lock);

        for (uint32_t i = 0; i < pending_count; i++)
            native_encoder_release_lease(router, &pending[i]);
        pthread_join(router->encoder_thread, NULL);
        LOG_I("native encoder handoff thread joined");
    }
    pthread_cond_destroy(&router->encoder_cond);
    pthread_mutex_destroy(&router->encoder_lock);
    pthread_mutex_destroy(&router->supervisor_send_lock);
    pthread_mutex_destroy(&router->audio_lock);

    LOG_I("output router destroyed (distributed: %lu, skipped: %lu, encoded: %lu, enc_dropped: %lu, previews: %lu, preview_dropped: %lu)",
          (unsigned long)router->frames_distributed,
          (unsigned long)router->frames_skipped,
          (unsigned long)router->encoder_frames_submitted,
          (unsigned long)router->encoder_frames_dropped,
          (unsigned long)router->preview_frames_submitted,
          (unsigned long)router->preview_frames_dropped);
    free(router);
}

/* ── Frame Distribution (called from main thread) ────────────── */

void sbs_output_router_distribute_frame(sbs_output_router_t *router)
{
    /* No-op: the export thread watches the compositor eventfd directly
     * and handles all frame export + distribution. This function is
     * retained for API compatibility but should not be called. */
    (void)router;
}

/* ── GLib I/O Callback ────────────────────────────────────────── */

gboolean sbs_output_router_on_frame_ready(GIOChannel *source,
                                           GIOCondition condition,
                                           gpointer user_data)
{
    (void)source;
    (void)condition;
    (void)user_data;
    /* No-op: export thread watches compositor eventfd directly */
    return G_SOURCE_CONTINUE;
}

gboolean sbs_output_router_on_frame_ready_fd(gint fd,
                                             GIOCondition condition,
                                             gpointer user_data)
{
    (void)fd;
    (void)condition;
    (void)user_data;
    /* No-op: export thread watches compositor eventfd directly */
    return G_SOURCE_CONTINUE;
}

/* ── Metrics ──────────────────────────────────────────────────── */

uint64_t sbs_output_router_frames_distributed(const sbs_output_router_t *router)
{
    return router ? router->frames_distributed : 0;
}

double sbs_output_router_last_encoder_time_ms(const sbs_output_router_t *router)
{
    sbs_output_router_t *mutable_router;
    double value;

    if (!router)
        return 0.0;

    mutable_router = (sbs_output_router_t *)router;
    pthread_mutex_lock(&mutable_router->encoder_lock);
    value = mutable_router->encoder_last_frame_time_ms;
    pthread_mutex_unlock(&mutable_router->encoder_lock);
    return value;
}

void sbs_output_router_set_preview_engine(sbs_output_router_t *router,
                                           sbs_preview_engine_t *preview)
{
    if (!router) {
        return;
    }
    pthread_mutex_lock(&router->preview_lock);
    router->preview = preview;
    pthread_mutex_unlock(&router->preview_lock);
}

void sbs_output_router_set_snapshot_engine(sbs_output_router_t *router,
                                           sbs_snapshot_engine_t *snapshot)
{
    if (!router) {
        return;
    }
    router->snapshot = snapshot;
}

void sbs_output_router_set_audio_mixer(sbs_output_router_t *router,
                                       sbs_audio_mixer_t *audio)
{
    if (!router) {
        return;
    }
    pthread_mutex_lock(&router->audio_lock);
    router->audio = audio;
    pthread_mutex_unlock(&router->audio_lock);
}

void sbs_output_router_set_encoder_manager(sbs_output_router_t *router,
                                            sbs_encoder_manager_t *encoder_mgr)
{
    if (!router) return;
    pthread_mutex_lock(&router->encoder_lock);
    router->encoder_mgr = encoder_mgr;
    router->encoder_last_content_frame = 0;
    pthread_mutex_unlock(&router->encoder_lock);
}

void sbs_output_router_set_color_mode(sbs_output_router_t *router,
                                       sbs_export_color_mode_t color_mode)
{
    if (!router) return;
    if (router->color_mode != color_mode) {
        LOG_I("color_mode changed: %d -> %d, resetting output export dest",
              router->color_mode, color_mode);
        router->color_mode = color_mode;
        /* Force the native preview ring to be rebuilt in the new color mode. */
        router->dest_output_inited = false;
        router->native_preview_configured = false;
        native_encoder_reset_content_gate(router);
    }
}
