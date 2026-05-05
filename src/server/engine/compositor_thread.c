#define SBS_LOG_COMP "comp-thread"

#include "sbs/compositor.h"
#include "sbs/compositor_ge2d.h"
#include "sbs/compositor_thread.h"
#include "sbs/frame_slot.h"
#include "sbs/ge2d.h"
#include "sbs/ipc.h"
#include "sbs/log.h"
#include "sbs/spsc_queue.h"
#include "sbs/types.h"

#include <pthread.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

static void bp_sync(void)
{
}

#ifndef DRM_FORMAT_NV21
#define DRM_FORMAT_NV21 0x3132564e
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x3031504e
#endif
#ifndef SBS_DRM_FORMAT_AMLY
#define SBS_DRM_FORMAT_AMLY 0x594c4d41u
#endif

#define SBS_COMP_QUEUE_SIZE 64

typedef enum {
    SBS_COMP_CMD_STOP,
    SBS_COMP_CMD_CLEAR_COLOR,
    SBS_COMP_CMD_SET_SCENE,
    SBS_COMP_CMD_SET_NATIVE_PREVIEW,
} sbs_comp_cmd_type_t;

typedef struct {
    sbs_comp_cmd_type_t type;
    float r, g, b, a;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t preview_frame_interval;
    sbs_comp_scene_state_t scene_state;
} sbs_comp_cmd_t;

typedef enum {
    SBS_COMP_PIPELINE_VULKAN_ONLY = 0,
    SBS_COMP_PIPELINE_GE2D_ONLY = 1,
    SBS_COMP_PIPELINE_HYBRID = 2,
} sbs_comp_pipeline_mode_t;

/* sbs_comp_event_t is defined in compositor_thread.h */

typedef struct sbs_compositor_thread {
    pthread_t           thread;
    sbs_compositor_t    compositor;
    sbs_spsc_queue_t   *cmd_queue;
    sbs_spsc_queue_t   *event_queue;
    int                 eventfd_fd;
    int                 stop_fd[2];
    volatile bool       running;
    bool                started;

    uint32_t            width;
    uint32_t            height;
    uint32_t            fps;
    sbs_export_color_mode_t color_mode;
    char               *shader_dir;

    uint64_t            frame_count;
    uint32_t            frames_dropped;
    sbs_comp_scene_state_t scene_state;
    bool                scene_dirty;
    sbs_comp_pipeline_mode_t pipeline_mode;
    sbs_ge2d_t          ge2d;
    int                 ge2d_source_fds[SBS_COMP_SCENE_MAX_ITEMS];
    const sbs_ge2d_buffer_t *last_ge2d_output;
    sbs_video_frame_msg_t last_frame_msg;
    bool                last_frame_ge2d;

    sbs_frame_fds_t    *source_release_pending[SBS_NATIVE_CANVAS_RING_SIZE][SBS_COMP_SCENE_MAX_ITEMS];
    uint32_t            source_release_pending_slot[SBS_NATIVE_CANVAS_RING_SIZE][SBS_COMP_SCENE_MAX_ITEMS];
    int                 source_release_pending_import_idx[SBS_NATIVE_CANVAS_RING_SIZE][SBS_COMP_SCENE_MAX_ITEMS];
    ino_t               source_release_pending_import_inode[SBS_NATIVE_CANVAS_RING_SIZE][SBS_COMP_SCENE_MAX_ITEMS];
    uint32_t            source_release_pending_count[SBS_NATIVE_CANVAS_RING_SIZE];

    double              last_frame_time_ms;
    double              total_frame_time_ms;
    double              min_frame_time_ms;
    double              max_frame_time_ms;
    uint64_t            timed_frame_count;
    int64_t             last_render_start_ns;
    int64_t             last_loop_start_ns;
    uint64_t            native_content_serial;
} sbs_compositor_thread_t;

static void close_ge2d_source_fds(sbs_compositor_thread_t *ct)
{
    uint32_t i;

    if (!ct)
        return;
    for (i = 0; i < SBS_COMP_SCENE_MAX_ITEMS; i++) {
        if (ct->ge2d_source_fds[i] >= 0) {
            close(ct->ge2d_source_fds[i]);
            ct->ge2d_source_fds[i] = -1;
        }
    }
}

static void init_ge2d_source_fds(sbs_compositor_thread_t *ct)
{
    uint32_t i;
    for (i = 0; i < SBS_COMP_SCENE_MAX_ITEMS; i++)
        ct->ge2d_source_fds[i] = -1;
}

static void release_pending_source_frames_for_entry(sbs_compositor_thread_t *ct,
                                                    uint32_t entry_idx)
{
    if (!ct || entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE)
        return;
    uint32_t count = ct->source_release_pending_count[entry_idx];
    for (uint32_t i = 0; i < count; i++) {
        sbs_compositor_release_source_dmabuf_import(&ct->compositor,
                                                    ct->source_release_pending_slot[entry_idx][i],
                                                    ct->source_release_pending_import_idx[entry_idx][i],
                                                    ct->source_release_pending_import_inode[entry_idx][i]);
        sbs_frame_fds_release(ct->source_release_pending[entry_idx][i], NULL);
        ct->source_release_pending[entry_idx][i] = NULL;
        ct->source_release_pending_slot[entry_idx][i] = 0;
        ct->source_release_pending_import_idx[entry_idx][i] = -1;
        ct->source_release_pending_import_inode[entry_idx][i] = 0;
    }
    ct->source_release_pending_count[entry_idx] = 0;
}

static void release_ready_source_frames(sbs_compositor_thread_t *ct)
{
    if (!ct || !ct->compositor.native_canvas.initialized)
        return;
    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        if (ct->source_release_pending_count[i] == 0)
            continue;
        sbs_native_canvas_entry_t *entry = &ct->compositor.native_canvas.entries[i];
        int state = atomic_load_explicit(&entry->state, memory_order_acquire);
        if (state != SBS_NATIVE_CANVAS_ENTRY_RENDERING ||
            vkGetFenceStatus(ct->compositor.device, entry->fence) == VK_SUCCESS)
            release_pending_source_frames_for_entry(ct, i);
    }
}

static void attach_source_releases_to_entry(sbs_compositor_thread_t *ct,
                                            uint32_t entry_idx,
                                            sbs_frame_fds_t **frames,
                                            const uint32_t *slots,
                                            const int *import_idxs,
                                            const ino_t *import_inodes,
                                            uint32_t count)
{
    if (!ct || !frames || entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE) {
        for (uint32_t i = 0; i < count; i++)
            sbs_frame_fds_release(frames[i], NULL);
        return;
    }

    release_pending_source_frames_for_entry(ct, entry_idx);
    if (count > SBS_COMP_SCENE_MAX_ITEMS)
        count = SBS_COMP_SCENE_MAX_ITEMS;
    for (uint32_t i = 0; i < count; i++) {
        ct->source_release_pending[entry_idx][i] = frames[i];
        ct->source_release_pending_slot[entry_idx][i] = slots ? slots[i] : 0;
        ct->source_release_pending_import_idx[entry_idx][i] = import_idxs ? import_idxs[i] : -1;
        ct->source_release_pending_import_inode[entry_idx][i] = import_inodes ? import_inodes[i] : 0;
    }
    ct->source_release_pending_count[entry_idx] = count;
}

static void release_all_pending_source_frames(sbs_compositor_thread_t *ct)
{
    if (!ct)
        return;
    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        release_pending_source_frames_for_entry(ct, i);
}

static bool item_has_vulkan_only_filters(const sbs_comp_scene_item_t *item)
{
    const uint32_t direct_yuv_filter_mask = SBS_COMP_FILTER_GRAYSCALE |
        SBS_COMP_FILTER_BRIGHTNESS | SBS_COMP_FILTER_CONTRAST |
        SBS_COMP_FILTER_HDR_TO_SDR_LUT | SBS_COMP_FILTER_COLOR_CORRECTION;

    if (!item || item->filter_flags == 0)
        return false;
    if ((item->filter_flags & ~direct_yuv_filter_mask) == 0 && item->lut_path[0] == '\0')
        return false;
    return true;
}

static const char *pipeline_mode_name(sbs_comp_pipeline_mode_t mode)
{
    switch (mode) {
    case SBS_COMP_PIPELINE_GE2D_ONLY:
        return "ge2d-only";
    case SBS_COMP_PIPELINE_HYBRID:
        return "hybrid";
    case SBS_COMP_PIPELINE_VULKAN_ONLY:
    default:
        return "vulkan-only";
    }
}

static uint32_t native_source_render_delay_us(void)
{
    static int initialized = 0;
    static uint32_t delay_us = 0;

    if (!initialized) {
        const char *env = getenv("SBS_NATIVE_SOURCE_RENDER_DELAY_US");
        if (env && env[0]) {
            unsigned long value = strtoul(env, NULL, 10);
            if (value > 20000ul)
                value = 20000ul;
            delay_us = (uint32_t)value;
            if (delay_us > 0)
                LOG_I("native source render delay enabled: %uus", delay_us);
        }
        initialized = 1;
    }

    return delay_us;
}

static int64_t monotonic_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

static void update_scene_transition(sbs_comp_scene_state_t *state)
{
    double elapsed_ms;

    if (!state || !state->transition_active)
        return;
    if (state->transition_duration_ms == 0 || state->transition_start_time_us <= 0) {
        state->transition_progress = 1.0f;
        state->transition_active = false;
        state->previous_item_count = 0;
        return;
    }

    elapsed_ms = (double)(monotonic_time_us() - state->transition_start_time_us) / 1000.0;
    state->transition_progress = (float)(elapsed_ms / (double)state->transition_duration_ms);
    if (state->transition_progress >= 1.0f) {
        state->transition_progress = 1.0f;
        state->transition_active = false;
        state->previous_item_count = 0;
    } else if (state->transition_progress < 0.0f) {
        state->transition_progress = 0.0f;
    }
}

static uint32_t source_texture_slot_for_active_item(const sbs_compositor_thread_t *ct,
                                                    const sbs_comp_scene_state_t *scene,
                                                    uint32_t item_idx)
{
    if (!scene || item_idx >= scene->active_item_count)
        return item_idx;

    const sbs_comp_scene_item_t *item = &scene->active_items[item_idx];
    uint32_t base_slot;
    if (scene->transition_active) {
        for (uint32_t i = 0; i < scene->previous_item_count && i < SBS_COMP_SCENE_MAX_ITEMS; i++) {
            const sbs_comp_scene_item_t *prev = &scene->previous_items[i];
            if (prev->visible && prev->frame_slot &&
                strcmp(prev->source_id, item->source_id) == 0) {
                return i;
            }
        }

        uint32_t slot = scene->previous_item_count;
        for (uint32_t i = 0; i < item_idx && i < SBS_COMP_SCENE_MAX_ITEMS; i++) {
            const sbs_comp_scene_item_t *prev_active = &scene->active_items[i];
            bool existed_in_previous = false;
            if (!prev_active->visible || !prev_active->frame_slot)
                continue;
            for (uint32_t j = 0; j < scene->previous_item_count && j < SBS_COMP_SCENE_MAX_ITEMS; j++) {
                const sbs_comp_scene_item_t *prev = &scene->previous_items[j];
                if (prev->visible && prev->frame_slot &&
                    strcmp(prev->source_id, prev_active->source_id) == 0) {
                    existed_in_previous = true;
                    break;
                }
            }
            if (!existed_in_previous)
                slot++;
        }
        base_slot = slot;
        goto resolve_slot;
    }

    for (uint32_t i = 0; i < item_idx && i < SBS_COMP_SCENE_MAX_ITEMS; i++) {
        const sbs_comp_scene_item_t *prev = &scene->active_items[i];
        if (prev->visible && prev->frame_slot &&
            strcmp(prev->source_id, item->source_id) == 0) {
            return i;
        }
    }
    base_slot = item_idx;

resolve_slot:
    if (!ct || base_slot >= SBS_MAX_SOURCE_TEXTURES || item->source_id[0] == '\0')
        return base_slot;
    if (ct->compositor.sources[base_slot].source_id[0] == '\0' ||
        strcmp(ct->compositor.sources[base_slot].source_id, item->source_id) == 0)
        return base_slot;
    for (uint32_t i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++) {
        if (strcmp(ct->compositor.sources[i].source_id, item->source_id) == 0)
            return i;
    }
    return base_slot;
}

static sbs_comp_pipeline_mode_t
sbs_compositor_analyze_scene(const sbs_compositor_thread_t *ct,
                             const sbs_comp_scene_state_t *state,
                             const char **reason_out)
{
    (void)ct;
    (void)state;
    if (reason_out)
        *reason_out = "vulkan-forced";
    return SBS_COMP_PIPELINE_VULKAN_ONLY;
}

static void update_pipeline_mode(sbs_compositor_thread_t *ct,
                                 const sbs_comp_scene_state_t *state)
{
    const char *reason = "unknown";
    sbs_comp_pipeline_mode_t next;

    next = sbs_compositor_analyze_scene(ct, state, &reason);
    if (next != ct->pipeline_mode) {
        LOG_I("pipeline mode %s -> %s (%s)",
              pipeline_mode_name(ct->pipeline_mode),
              pipeline_mode_name(next),
              reason);
        ct->pipeline_mode = next;
    }
}

static void write_eventfd(int fd)
{
    uint64_t val = 1;
    if (write(fd, &val, sizeof(val)) < 0 && errno != EAGAIN)
        LOG_W("eventfd write failed: %s", strerror(errno));
}

static void drain_eventfd(int fd)
{
    uint64_t val;
    while (read(fd, &val, sizeof(val)) > 0)
        ;
}

static void *compositor_thread_func(void *arg)
{
    sbs_compositor_thread_t *ct = arg;

    LOG_I("compositor thread starting (init Vulkan %ux%u @ %u fps)",
          ct->width, ct->height, ct->fps);
    bp_sync();

    if (sbs_compositor_init(&ct->compositor, ct->width, ct->height,
                            ct->color_mode) != 0) {
        LOG_E("compositor init failed in thread");
        write_eventfd(ct->eventfd_fd);
        return NULL;
    }
    LOG_I("Vulkan compositor init complete");
    bp_sync();

    if (getenv("SBS_NO_GE2D")) {
        LOG_I("GE2D disabled via SBS_NO_GE2D env var");
    } else if (sbs_ge2d_init(&ct->ge2d) != SBS_OK) {
        LOG_W("GE2D init failed, continuing Vulkan-only");
    }
    if (getenv("SBS_GE2D_INIT_ONLY") && ct->ge2d.available) {
        LOG_I("SBS_GE2D_INIT_ONLY: GE2D opened but will force vulkan-only pipeline");
        ct->ge2d.available = false;
    }
    if (getenv("SBS_GE2D_ALLOC_ONLY") && ct->ge2d.available) {
        LOG_I("SBS_GE2D_ALLOC_ONLY: allocating pool, then disabling GE2D ops");
        bp_sync();
        if (sbs_ge2d_pool_init(&ct->ge2d, ct->width, ct->height) == SBS_OK) {
            LOG_I("SBS_GE2D_ALLOC_ONLY: pool allocated OK, disabling GE2D for composition");
        } else {
            LOG_W("SBS_GE2D_ALLOC_ONLY: pool alloc failed");
        }
        bp_sync();
        ct->ge2d.available = false;
    }
    if (getenv("SBS_GE2D_TEST_FILLRECT") && ct->ge2d.available) {
        LOG_I("[TEST] SBS_GE2D_TEST_FILLRECT: running single fillrect test");
        bp_sync();
        int test_rc = sbs_ge2d_test_fillrect(&ct->ge2d);
        LOG_I("[TEST] SBS_GE2D_TEST_FILLRECT result=%d (%s)",
              test_rc, test_rc == SBS_OK ? "SUCCESS" : "FAILED");
        bp_sync();
        ct->ge2d.available = false;
    }
    LOG_I("GE2D init phase complete (available=%d)", ct->ge2d.available);
    bp_sync();

    if (ct->shader_dir && sbs_compositor_load_shaders(&ct->compositor, ct->shader_dir) != 0) {
        LOG_W("shader loading failed, continuing with clear-only");
    }

    /* Placeholder texture is now created during sbs_compositor_init() */

    LOG_I("compositor thread entering render loop");
    bp_sync();

    int64_t frame_interval_ns = (int64_t)(1000000000LL / ct->fps);
    sbs_comp_scene_state_init(&ct->scene_state);
    ct->scene_state.background_rgba[0] = 0.1f;
    ct->scene_state.background_rgba[1] = 0.1f;
    ct->scene_state.background_rgba[2] = 0.1f;
    ct->scene_dirty = true;
    ct->pipeline_mode = SBS_COMP_PIPELINE_VULKAN_ONLY;
    init_ge2d_source_fds(ct);
    sbs_video_frame_msg_init(&ct->last_frame_msg, SBS_IPC_MSG_COMPOSED_FRAME);
    ct->min_frame_time_ms = 1e9;
    ct->max_frame_time_ms = 0;

    struct timespec next_deadline;
    clock_gettime(CLOCK_MONOTONIC, &next_deadline);
    ct->last_loop_start_ns = next_deadline.tv_sec * 1000000000LL + next_deadline.tv_nsec;

    while (ct->running) {
        sbs_comp_cmd_t *cmd;
        while ((cmd = sbs_spsc_queue_pop(ct->cmd_queue)) != NULL) {
            if (cmd->type == SBS_COMP_CMD_STOP) {
                free(cmd);
                ct->running = false;
                goto done;
            }
            if (cmd->type == SBS_COMP_CMD_CLEAR_COLOR) {
                ct->scene_state.background_rgba[0] = cmd->r;
                ct->scene_state.background_rgba[1] = cmd->g;
                ct->scene_state.background_rgba[2] = cmd->b;
                ct->scene_state.background_rgba[3] = cmd->a;
                ct->scene_dirty = true;
            }
            if (cmd->type == SBS_COMP_CMD_SET_SCENE) {
                int64_t previous_transition_start_us = ct->scene_state.transition_start_time_us;
                close_ge2d_source_fds(ct);
                ct->scene_state = cmd->scene_state;
                if (ct->scene_state.transition_active &&
                    ct->scene_state.transition_start_time_us != previous_transition_start_us) {
                    ct->scene_state.transition_start_time_us = monotonic_time_us();
                    ct->scene_state.transition_progress = 0.0f;
                }
                ct->scene_dirty = true;
                update_pipeline_mode(ct, &ct->scene_state);
                if (ct->scene_state.active_item_count > 0) {
                    const sbs_comp_scene_item_t *item = &ct->scene_state.active_items[0];
                    LOG_I("comp-thread scene applied item=%s flags=0x%x tint=(%.3f,%.3f,%.3f)",
                          item->source_id,
                          item->filter_flags,
                          item->tint[0], item->tint[1], item->tint[2]);
                }
            }
            if (cmd->type == SBS_COMP_CMD_SET_NATIVE_PREVIEW) {
                if (sbs_compositor_configure_native_preview(&ct->compositor,
                                                            cmd->preview_width,
                                                            cmd->preview_height,
                                                            cmd->preview_frame_interval) != 0) {
                    LOG_W("native preview configure failed: %ux%u interval=%u",
                          cmd->preview_width, cmd->preview_height,
                          cmd->preview_frame_interval);
                }
                ct->scene_dirty = true;
            }
            free(cmd);
        }

        update_scene_transition(&ct->scene_state);

        release_ready_source_frames(ct);

        /* Wait for the current render target to become reusable.
         * This covers both the render fence and the latest async export that
         * sampled from the same target on the graphics queue. */
        uint32_t idx = ct->compositor.current_target;
        VkResult fence_res = vkWaitForFences(ct->compositor.device, 1,
                                              &ct->compositor.targets[idx].fence,
                                              VK_TRUE, 2000000ULL);
        if (fence_res == VK_NOT_READY || fence_res == VK_TIMEOUT) {
            ct->frames_dropped++;
            usleep(1000);
            continue;
        }
        if (fence_res != VK_SUCCESS) {
            LOG_W("render target %u fence wait failed (res=%d)", idx, fence_res);
            ct->frames_dropped++;
            usleep(1000);
            continue;
        }

        sbs_export_slot_t *retire_slot = ct->compositor.targets[idx].retire_slot;
        if (retire_slot) {
            /* Hold retire_slot_mutex for the entire read/deref/clear sequence
             * so teardown on the output_router thread cannot free the slot
             * from under us. Teardown runs vkQueueWaitIdle first, so the
             * vkWaitForFences below cannot block teardown indefinitely. */
            pthread_mutex_lock(&ct->compositor.retire_slot_mutex);
            /* Re-read under the lock — teardown may have cleared it. */
            retire_slot = ct->compositor.targets[idx].retire_slot;
            if (!retire_slot) {
                pthread_mutex_unlock(&ct->compositor.retire_slot_mutex);
                goto retire_done;
            }
            /* HDR10/P010 compute is much heavier than NV21; use a longer timeout
             * to avoid cascading frame drops when the GPU is loaded. */
            uint64_t retire_timeout_ns = 2000000ULL;
            if (retire_slot->nv21_size > (VkDeviceSize)ct->width * ct->height * 2) {
                /* P010 buffer is 4 bytes/pixel vs NV21 1.5 bytes/pixel */
                retire_timeout_ns = 100000000ULL; /* 100ms for HDR10 */
            }
            VkResult retire_res = vkWaitForFences(ct->compositor.device, 1,
                                                  &retire_slot->fence,
                                                  VK_TRUE, retire_timeout_ns);
            if (retire_res == VK_NOT_READY || retire_res == VK_TIMEOUT) {
                pthread_mutex_unlock(&ct->compositor.retire_slot_mutex);
                ct->frames_dropped++;
                usleep(1000);
                continue;
            }
            if (retire_res != VK_SUCCESS) {
                pthread_mutex_unlock(&ct->compositor.retire_slot_mutex);
                LOG_W("render target %u export retire wait failed (res=%d)",
                      idx, retire_res);
                ct->frames_dropped++;
                usleep(1000);
                continue;
            }

            uint32_t refs = atomic_load(&retire_slot->retire_refs);
            if (refs > 0)
                refs = atomic_fetch_sub(&retire_slot->retire_refs, 1) - 1;
            if (refs == 0 && retire_slot->release_pending_free)
                atomic_store(&retire_slot->state, SBS_EXPORT_SLOT_FREE);
            ct->compositor.targets[idx].retire_slot = NULL;
            pthread_mutex_unlock(&ct->compositor.retire_slot_mutex);
        }
retire_done:

        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        if (ct->pipeline_mode == SBS_COMP_PIPELINE_GE2D_ONLY) {
            const sbs_ge2d_buffer_t *out_buffer = NULL;

            for (uint32_t i = 0; i < ct->scene_state.active_item_count; i++) {
                sbs_comp_scene_item_t *item = &ct->scene_state.active_items[i];
                uint64_t ts = 0;
                sbs_frame_fds_t *fds;
                int fd;

                if (!item->frame_slot || !item->visible)
                    continue;
                fds = (sbs_frame_fds_t *)sbs_frame_slot_take_oldest(item->frame_slot, &ts);
                if (!fds)
                    continue;
                fd = fds->dmabuf_fd;
                if (fd > 0) {
                    if (ct->ge2d_source_fds[i] >= 0)
                        close(ct->ge2d_source_fds[i]);
                    ct->ge2d_source_fds[i] = fd;
                    fds->dmabuf_fd = -1;
                }
                sbs_frame_fds_release(fds, NULL);
            }

            if (sbs_compositor_ge2d_compose_frame(&ct->ge2d,
                                                  ct->width,
                                                  ct->height,
                                                  &ct->scene_state,
                                                  ct->ge2d_source_fds,
                                                  SBS_COMP_SCENE_MAX_ITEMS,
                                                  &out_buffer) != SBS_OK) {
                ct->frames_dropped++;
                usleep(1000);
                continue;
            }

            ct->last_ge2d_output = out_buffer;
            ct->last_frame_ge2d = true;
            sbs_video_frame_msg_init(&ct->last_frame_msg, SBS_IPC_MSG_COMPOSED_FRAME);
            ct->last_frame_msg.width = out_buffer->width;
            ct->last_frame_msg.height = out_buffer->height;
            ct->last_frame_msg.drm_format = DRM_FORMAT_NV21;
            ct->last_frame_msg.drm_modifier = 0;
            ct->last_frame_msg.n_planes = 2;
            ct->last_frame_msg.plane_offset[0] = 0;
            ct->last_frame_msg.plane_offset[1] = out_buffer->stride * out_buffer->height;
            ct->last_frame_msg.plane_stride[0] = out_buffer->stride;
            ct->last_frame_msg.plane_stride[1] = out_buffer->stride;
            ct->last_frame_msg.buffer_type = SBS_FRAME_BUFFER_DMABUF;
            ct->last_frame_msg.flags = 0;
        } else {
            /* Poll frame_slots for each active scene item and upload new frames. */
            struct timespec ts_upload_start, ts_upload_end, ts_render_start, ts_render_end;
            double upload_ms = 0.0;
            int uploaded_count = 0;
            int uploaded_success_count = 0;
            bool native_repeated = false;
            int native_render_rc = -1;
            sbs_frame_fds_t *source_releases[SBS_COMP_SCENE_MAX_ITEMS] = {0};
            uint32_t source_release_slots[SBS_COMP_SCENE_MAX_ITEMS] = {0};
            int source_release_import_idxs[SBS_COMP_SCENE_MAX_ITEMS];
            ino_t source_release_import_inodes[SBS_COMP_SCENE_MAX_ITEMS] = {0};
            uint32_t source_release_count = 0;

            for (uint32_t i = 0; i < SBS_COMP_SCENE_MAX_ITEMS; i++)
                source_release_import_idxs[i] = -1;

            clock_gettime(CLOCK_MONOTONIC, &ts_upload_start);
            for (uint32_t i = 0; i < ct->scene_state.active_item_count; i++) {
                sbs_comp_scene_item_t *item = &ct->scene_state.active_items[i];
                uint32_t source_slot = source_texture_slot_for_active_item(ct, &ct->scene_state, i);
                bool duplicate_source = false;
                for (uint32_t j = 0; j < i; j++) {
                    sbs_comp_scene_item_t *prev_item = &ct->scene_state.active_items[j];
                    if (prev_item->visible && prev_item->frame_slot &&
                        strcmp(prev_item->source_id, item->source_id) == 0) {
                        duplicate_source = true;
                        break;
                    }
                }
                if (duplicate_source)
                    continue;
                if (!item->frame_slot || !item->visible)
                    continue;
                if (item->frame_width == 0 || item->frame_height == 0)
                    continue;

                uint64_t ts = 0;
                sbs_frame_fds_t *fds = (sbs_frame_fds_t *)sbs_frame_slot_take_oldest(item->frame_slot, &ts);
                if (!fds)
                    continue;

                int fd = fds->dmabuf_fd;
                int fd2 = fds->dmabuf_fd2;
                if (fd <= 0) {
                    sbs_frame_fds_release(fds, NULL);
                    continue;
                }

                int rc = sbs_compositor_upload_source(&ct->compositor, source_slot,
                                                       item->source_id,
                                                       fd, fd2,
                                                       item->frame_width,
                                                       item->frame_height,
                                                       item->drm_format,
                                                       item->drm_modifier,
                                                       item->plane_offset,
                                                       item->plane_stride,
                                                       item_has_vulkan_only_filters(item),
                                                       item->filter_flags);
                uploaded_count++;
                if (rc != 0) {
                    LOG_W("source upload failed for item %u ('%s')", source_slot, item->source_id);
                    sbs_frame_fds_release(fds, NULL);
                } else if (ct->compositor.native_canvas.initialized &&
                           source_release_count < SBS_COMP_SCENE_MAX_ITEMS) {
                    uploaded_success_count++;
                    int import_idx = -1;
                    ino_t import_inode = 0;
                    sbs_source_texture_t *tex = &ct->compositor.sources[source_slot];
                    if (tex->drm_format == SBS_DRM_FORMAT_AMLY &&
                        tex->dmabuf_buf_active_idx >= 0 &&
                        tex->dmabuf_buf_active_idx < SBS_DMABUF_BUF_CACHE_SIZE) {
                        int active_idx = tex->dmabuf_buf_active_idx;
                        if (tex->dmabuf_buf_cache[active_idx].valid &&
                            tex->dmabuf_buf_cache[active_idx].drm_format == SBS_DRM_FORMAT_AMLY) {
                            import_idx = active_idx;
                            import_inode = tex->dmabuf_buf_cache[active_idx].inode;
                        }
                    }
                    source_release_slots[source_release_count] = source_slot;
                    source_release_import_idxs[source_release_count] = import_idx;
                    source_release_import_inodes[source_release_count] = import_inode;
                    source_releases[source_release_count++] = fds;
                } else {
                    uploaded_success_count++;
                    sbs_frame_fds_release(fds, NULL);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &ts_upload_end);
            upload_ms = (double)(ts_upload_end.tv_sec - ts_upload_start.tv_sec) * 1000.0
                      + (double)(ts_upload_end.tv_nsec - ts_upload_start.tv_nsec) / 1000000.0;

            uint32_t render_delay_us = native_source_render_delay_us();
            if (uploaded_success_count > 0 && render_delay_us > 0)
                usleep(render_delay_us);

            clock_gettime(CLOCK_MONOTONIC, &ts_render_start);
            if (ct->compositor.native_canvas.initialized) {
                uint32_t submitted_entry_idx = UINT32_MAX;
                uint64_t frame_number = ct->frame_count + 1u;
                bool content_changed = uploaded_success_count > 0 || ct->scene_dirty ||
                    ct->scene_state.transition_active;
                uint64_t content_frame_number = ct->native_content_serial;
                int render_rc = -1;
                if (content_changed || content_frame_number == 0)
                    content_frame_number = ct->native_content_serial + 1u;
                if (!ct->scene_state.transition_active &&
                    uploaded_count == 0 && !ct->scene_dirty) {
                    native_repeated = sbs_compositor_native_canvas_republish_last(&ct->compositor,
                                                                                   frame_number);
                }
                if (!native_repeated) {
                    render_rc = sbs_compositor_render_native_frame(&ct->compositor,
                                                                   &ct->scene_state,
                                                                   frame_number,
                                                                   content_frame_number,
                                                                   &submitted_entry_idx);
                    if (render_rc == 0) {
                        ct->native_content_serial = content_frame_number;
                        ct->scene_dirty = false;
                    }
                }
                native_render_rc = render_rc;
                if (render_rc == 0 && submitted_entry_idx < SBS_NATIVE_CANVAS_RING_SIZE) {
                    attach_source_releases_to_entry(ct, submitted_entry_idx,
                                                    source_releases,
                                                    source_release_slots,
                                                    source_release_import_idxs,
                                                    source_release_import_inodes,
                                                    source_release_count);
                    source_release_count = 0;
                }
            } else {
                sbs_compositor_render_frame(&ct->compositor, &ct->scene_state);
            }
            for (uint32_t i = 0; i < source_release_count; i++)
                sbs_frame_fds_release(source_releases[i], NULL);
            clock_gettime(CLOCK_MONOTONIC, &ts_render_end);
            double render_ms = (double)(ts_render_end.tv_sec - ts_render_start.tv_sec) * 1000.0
                             + (double)(ts_render_end.tv_nsec - ts_render_start.tv_nsec) / 1000000.0;
            ct->last_frame_ge2d = false;

            if (ct->frame_count % 60 == 0 && (upload_ms > 10.0 || render_ms > 10.0)) {
                LOG_I("render breakdown: frame=%lu upload=%.2fms render=%.2fms uploaded=%d ok=%d repeated=%d rc=%d",
                      (unsigned long)ct->frame_count, upload_ms, render_ms,
                      uploaded_count, uploaded_success_count,
                      native_repeated ? 1 : 0, native_render_rc);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        {
            double frame_ms = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000.0
                            + (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000000.0;
            ct->last_frame_time_ms = frame_ms;
            ct->total_frame_time_ms += frame_ms;
            ct->timed_frame_count++;
            if (frame_ms < ct->min_frame_time_ms) ct->min_frame_time_ms = frame_ms;
            if (frame_ms > ct->max_frame_time_ms) ct->max_frame_time_ms = frame_ms;
        }

        ct->frame_count++;

        sbs_comp_event_t *ev = malloc(sizeof(sbs_comp_event_t));
        if (ev) {
            ev->frame_number = ct->frame_count;
            ev->frames_dropped = ct->frames_dropped;
            if (sbs_spsc_queue_push(ct->event_queue, ev))
                write_eventfd(ct->eventfd_fd);
            else
                free(ev);
        }

        /* Deadline-based sleep: advance next_deadline by one frame interval
         * and sleep only the remaining time. This ensures we maintain the
         * target framerate regardless of how long the render took. */
        next_deadline.tv_nsec += frame_interval_ns;
        while (next_deadline.tv_nsec >= 1000000000L) {
            next_deadline.tv_nsec -= 1000000000L;
            next_deadline.tv_sec++;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remain_ns = (next_deadline.tv_sec - now.tv_sec) * 1000000000LL
                          + (next_deadline.tv_nsec - now.tv_nsec);
        if (remain_ns > 1000000LL) {
            /* Sleep remaining time (min 1ms to avoid busy spin from rounding) */
            usleep((useconds_t)(remain_ns / 1000));
        } else if (remain_ns < -frame_interval_ns) {
            /* We're more than a full frame behind — reset deadline to now
             * to avoid a burst of catch-up renders */
            clock_gettime(CLOCK_MONOTONIC, &next_deadline);
        }
        /* else: we're late but within one frame — skip sleep, render immediately */

        /* Debug: measure total loop time */
        struct timespec loop_now;
        clock_gettime(CLOCK_MONOTONIC, &loop_now);
        int64_t loop_start_ns = loop_now.tv_sec * 1000000000LL + loop_now.tv_nsec;
        if (ct->last_loop_start_ns > 0 && ct->frame_count % 60 == 0) {
            double loop_ms = (double)(loop_start_ns - ct->last_loop_start_ns) / 1000000.0;
            LOG_I("loop timing: frame=%lu render=%.2fms remain_ns=%ld loop_ms=%.2f",
                  (unsigned long)ct->frame_count, ct->last_frame_time_ms,
                  (long)remain_ns, loop_ms);
        }
        ct->last_loop_start_ns = loop_start_ns;
    }

done:
    LOG_I("compositor thread exiting (rendered %lu frames, dropped %u)",
          (unsigned long)ct->frame_count, ct->frames_dropped);
    release_all_pending_source_frames(ct);
    close_ge2d_source_fds(ct);
    sbs_ge2d_shutdown(&ct->ge2d);
    sbs_compositor_destroy(&ct->compositor);
    return NULL;
}

sbs_compositor_thread_t *sbs_compositor_thread_new(uint32_t width,
                                                    uint32_t height,
                                                    uint32_t fps,
                                                    sbs_export_color_mode_t color_mode,
                                                    const char *shader_dir)
{
    sbs_compositor_thread_t *ct = calloc(1, sizeof(*ct));
    if (!ct)
        return NULL;

    ct->width = width;
    ct->height = height;
    ct->fps = fps;
    ct->color_mode = color_mode;
    ct->shader_dir = shader_dir ? strdup(shader_dir) : NULL;
    ct->eventfd_fd = -1;
    ct->stop_fd[0] = -1;
    ct->stop_fd[1] = -1;

    return ct;
}

void sbs_compositor_thread_free(sbs_compositor_thread_t *ct)
{
    if (!ct)
        return;

    if (ct->started)
        sbs_compositor_thread_stop(ct);

    free(ct->shader_dir);
    free(ct);
}

int sbs_compositor_thread_start(sbs_compositor_thread_t *ct)
{
    if (ct->started)
        return 0;

    ct->cmd_queue = sbs_spsc_queue_new(SBS_COMP_QUEUE_SIZE);
    if (!ct->cmd_queue) {
        LOG_E("cmd queue alloc failed");
        return -1;
    }

    ct->event_queue = sbs_spsc_queue_new(SBS_COMP_QUEUE_SIZE);
    if (!ct->event_queue) {
        LOG_E("event queue alloc failed");
        sbs_spsc_queue_free(ct->cmd_queue);
        return -1;
    }

    ct->eventfd_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ct->eventfd_fd < 0) {
        LOG_E("eventfd creation failed: %s", strerror(errno));
        sbs_spsc_queue_free(ct->cmd_queue);
        sbs_spsc_queue_free(ct->event_queue);
        return -1;
    }

    if (pipe(ct->stop_fd) != 0) {
        LOG_E("stop pipe creation failed: %s", strerror(errno));
        close(ct->eventfd_fd);
        sbs_spsc_queue_free(ct->cmd_queue);
        sbs_spsc_queue_free(ct->event_queue);
        return -1;
    }

    ct->running = true;

    int res = pthread_create(&ct->thread, NULL, compositor_thread_func, ct);
    if (res != 0) {
        LOG_E("pthread_create failed: %s", strerror(res));
        close(ct->stop_fd[0]);
        close(ct->stop_fd[1]);
        close(ct->eventfd_fd);
        sbs_spsc_queue_free(ct->cmd_queue);
        sbs_spsc_queue_free(ct->event_queue);
        ct->running = false;
        return -1;
    }

    ct->started = true;
    LOG_I("compositor thread started");
    return 0;
}

int sbs_compositor_thread_stop(sbs_compositor_thread_t *ct)
{
    if (!ct->started)
        return 0;

    sbs_comp_cmd_t *cmd = malloc(sizeof(sbs_comp_cmd_t));
    if (cmd) {
        cmd->type = SBS_COMP_CMD_STOP;
        sbs_spsc_queue_push(ct->cmd_queue, cmd);
    }

    ct->running = false;
    uint8_t byte = 1;
    ssize_t wr __attribute__((unused)) = write(ct->stop_fd[1], &byte, 1);

    int res = pthread_join(ct->thread, NULL);
    if (res != 0) {
        LOG_E("pthread_join failed: %s", strerror(res));
        return -1;
    }

    sbs_comp_cmd_t *c;
    while ((c = sbs_spsc_queue_pop(ct->cmd_queue)) != NULL)
        free(c);

    sbs_comp_event_t *e;
    while ((e = sbs_spsc_queue_pop(ct->event_queue)) != NULL)
        free(e);

    sbs_spsc_queue_free(ct->cmd_queue);
    sbs_spsc_queue_free(ct->event_queue);
    close(ct->stop_fd[0]);
    close(ct->stop_fd[1]);
    close(ct->eventfd_fd);
    ct->eventfd_fd = -1;
    ct->started = false;

    LOG_I("compositor thread stopped (total frames: %lu, dropped: %u)",
          (unsigned long)ct->frame_count, ct->frames_dropped);
    return 0;
}

int sbs_compositor_thread_get_eventfd(sbs_compositor_thread_t *ct)
{
    return ct->eventfd_fd;
}

void sbs_compositor_thread_get_stats(sbs_compositor_thread_t *ct,
                                     uint64_t *frame_count,
                                     uint32_t *frames_dropped)
{
    if (!ct) {
        return;
    }
    if (frame_count) {
        *frame_count = ct->frame_count;
    }
    if (frames_dropped) {
        *frames_dropped = ct->frames_dropped;
    }
}

void sbs_compositor_thread_get_timing(sbs_compositor_thread_t *ct,
                                      sbs_comp_timing_stats_t *out)
{
    if (!ct || !out) return;
    memset(out, 0, sizeof(*out));
    out->frame_count = ct->frame_count;
    out->content_frame_count = ct->compositor.native_canvas.initialized
        ? ct->native_content_serial
        : ct->frame_count;
    out->repeated_frame_count = out->frame_count > out->content_frame_count
        ? out->frame_count - out->content_frame_count
        : 0;
    out->frames_dropped = ct->frames_dropped;
    out->last_frame_time_ms = ct->last_frame_time_ms;
    out->min_frame_time_ms = ct->min_frame_time_ms;
    out->max_frame_time_ms = ct->max_frame_time_ms;
    if (ct->timed_frame_count > 0) {
        out->avg_frame_time_ms = ct->total_frame_time_ms / (double)ct->timed_frame_count;
    }
    if (out->avg_frame_time_ms > 0) {
        out->fps_actual = 1000.0 / out->avg_frame_time_ms;
    }
}

int sbs_compositor_thread_set_scene(sbs_compositor_thread_t *ct,
                                    const sbs_comp_scene_state_t *state)
{
    sbs_comp_cmd_t *cmd;

    if (!ct || !state || !ct->cmd_queue) {
        return -1;
    }

    cmd = calloc(1, sizeof(*cmd));
    if (!cmd) {
        return -1;
    }
    cmd->type = SBS_COMP_CMD_SET_SCENE;
    cmd->scene_state = *state;

    if (!sbs_spsc_queue_push(ct->cmd_queue, cmd)) {
        free(cmd);
        return -1;
    }

    return 0;
}

int sbs_compositor_thread_configure_native_preview(sbs_compositor_thread_t *ct,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   uint32_t frame_interval)
{
    sbs_comp_cmd_t *cmd;

    if (!ct || !ct->cmd_queue)
        return -1;
    if ((width == 0) != (height == 0))
        return -1;

    cmd = calloc(1, sizeof(*cmd));
    if (!cmd)
        return -1;
    cmd->type = SBS_COMP_CMD_SET_NATIVE_PREVIEW;
    cmd->preview_width = width;
    cmd->preview_height = height;
    cmd->preview_frame_interval = frame_interval > 0 ? frame_interval : 1;

    if (!sbs_spsc_queue_push(ct->cmd_queue, cmd)) {
        free(cmd);
        return -1;
    }

    return 0;
}

bool sbs_compositor_thread_pop_event(sbs_compositor_thread_t *ct,
                                      sbs_comp_event_t **ev)
{
    void *item = sbs_spsc_queue_pop(ct->event_queue);
    if (!item)
        return false;
    *ev = (sbs_comp_event_t *)item;
    return true;
}

void sbs_compositor_thread_drain_events(sbs_compositor_thread_t *ct)
{
    drain_eventfd(ct->eventfd_fd);
}

int sbs_compositor_thread_export_frame(sbs_compositor_thread_t *ct,
                                       sbs_video_frame_msg_t *msg,
                                       int *fd)
{
    if (!ct || !fd || !msg) {
        if (fd) *fd = -1;
        return -1;
    }

    if (ct->last_frame_ge2d) {
        if (!ct->last_ge2d_output || ct->last_ge2d_output->fd < 0) {
            *fd = -1;
            return -1;
        }
        *msg = ct->last_frame_msg;
        *fd = dup(ct->last_ge2d_output->fd);
        return *fd >= 0 ? 0 : -1;
    }

    /* Read the most recently completed render target (set atomically by
     * the compositor thread after vkQueueSubmit). */
    uint32_t last_rendered = atomic_load(&ct->compositor.last_rendered_target);

    sbs_video_frame_msg_init(msg, SBS_IPC_MSG_COMPOSED_FRAME);
    msg->width = ct->width;
    msg->height = ct->height;
    msg->drm_format = DRM_FORMAT_NV21;
    msg->drm_modifier = 0;
    msg->n_planes = 2;
    msg->plane_offset[0] = 0;
    msg->plane_offset[1] = ct->width * ct->height;
    msg->plane_stride[0] = ct->width;
    msg->plane_stride[1] = ct->width;
    msg->buffer_type = ct->compositor.dmabuf_export_available
        ? SBS_FRAME_BUFFER_DMABUF
        : SBS_FRAME_BUFFER_MEMFD;
    msg->flags = 0;

    return sbs_compositor_export_target_fd(&ct->compositor, last_rendered, fd);
}

int sbs_compositor_thread_export_frame_ptr(sbs_compositor_thread_t *ct,
                                            sbs_video_frame_msg_t *msg,
                                            const void **data_out,
                                            size_t *size_out,
                                            uint32_t *target_idx_out)
{
    if (!ct || !msg || !data_out || !size_out || !target_idx_out)
        return -1;

    if (!ct->compositor.nv21_compute_available)
        return -1;

    uint32_t last_rendered = atomic_load(&ct->compositor.last_rendered_target);

    sbs_video_frame_msg_init(msg, SBS_IPC_MSG_COMPOSED_FRAME);
    msg->width = ct->width;
    msg->height = ct->height;
    msg->drm_format = DRM_FORMAT_NV21;
    msg->drm_modifier = 0;
    msg->n_planes = 2;
    msg->plane_offset[0] = 0;
    msg->plane_offset[1] = ct->width * ct->height;
    msg->plane_stride[0] = ct->width;
    msg->plane_stride[1] = ct->width;
    msg->buffer_type = SBS_FRAME_BUFFER_MEMFD;
    msg->flags = 0;

    return sbs_compositor_export_target_nv21_ptr(&ct->compositor, last_rendered,
                                                  data_out, size_out, target_idx_out);
}

int sbs_compositor_thread_export_preview_frame(sbs_compositor_thread_t *ct,
                                               uint32_t width,
                                               uint32_t height,
                                               sbs_video_frame_msg_t *msg,
                                               int *fd)
{
    uint32_t last_rendered;

    if (!ct || !fd || !msg || width == 0 || height == 0) {
        if (fd) *fd = -1;
        return -1;
    }

    if (ct->last_frame_ge2d) {
        return sbs_compositor_thread_export_frame(ct, msg, fd);
    }

    last_rendered = atomic_load(&ct->compositor.last_rendered_target);

    sbs_video_frame_msg_init(msg, SBS_IPC_MSG_COMPOSED_FRAME);
    msg->width = width;
    msg->height = height;
    msg->drm_format = DRM_FORMAT_NV21;
    msg->drm_modifier = 0;
    msg->n_planes = 2;
    msg->plane_offset[0] = 0;
    msg->plane_offset[1] = width * height;
    msg->plane_stride[0] = width;
    msg->plane_stride[1] = width;
    msg->buffer_type = SBS_FRAME_BUFFER_MEMFD;
    msg->flags = 0;

    return sbs_compositor_export_target_preview_fd(&ct->compositor, last_rendered,
                                                   width, height, fd);
}

int sbs_compositor_thread_export_preview_frame_ptr(sbs_compositor_thread_t *ct,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    sbs_video_frame_msg_t *msg,
                                                    const void **data_out,
                                                    size_t *size_out)
{
    if (!ct || !msg || !data_out || !size_out || width == 0 || height == 0)
        return -1;

    uint32_t last_rendered = atomic_load(&ct->compositor.last_rendered_target);

    sbs_video_frame_msg_init(msg, SBS_IPC_MSG_COMPOSED_FRAME);
    msg->width = width;
    msg->height = height;
    msg->drm_format = DRM_FORMAT_NV21;
    msg->drm_modifier = 0;
    msg->n_planes = 2;
    msg->plane_offset[0] = 0;
    msg->plane_offset[1] = width * height;
    msg->plane_stride[0] = width;
    msg->plane_stride[1] = width;
    msg->buffer_type = SBS_FRAME_BUFFER_MEMFD;
    msg->flags = 0;

    return sbs_compositor_export_target_preview_ptr(&ct->compositor, last_rendered,
                                                     width, height, data_out, size_out);
}

bool sbs_compositor_thread_can_export_dmabuf(sbs_compositor_thread_t *ct)
{
    if (!ct) {
        return false;
    }
    return ct->last_frame_ge2d || ct->compositor.dmabuf_export_available;
}

/* ── Destination-oriented export wrappers ─────────────────────── */

int sbs_compositor_thread_init_export_dest(sbs_compositor_thread_t *ct,
                                            sbs_export_dest_type_t type,
                                            uint32_t width, uint32_t height,
                                            sbs_export_color_mode_t color_mode)
{
    if (!ct) return -1;
    return sbs_compositor_init_export_dest(&ct->compositor, type, width, height, color_mode);
}

int sbs_compositor_thread_export_dest_submit(sbs_compositor_thread_t *ct,
                                              sbs_export_dest_type_t type,
                                              uint64_t frame_number)
{
    if (!ct) return -1;
    uint32_t last_rendered = atomic_load(&ct->compositor.last_rendered_target);
    return sbs_compositor_export_dest_submit(&ct->compositor, type, last_rendered, frame_number);
}

sbs_export_slot_t *sbs_compositor_thread_export_dest_acquire(sbs_compositor_thread_t *ct,
                                                              sbs_export_dest_type_t type)
{
    if (!ct) return NULL;
    return sbs_compositor_export_dest_acquire(&ct->compositor, type);
}

void sbs_compositor_thread_export_dest_release(sbs_compositor_thread_t *ct,
                                                sbs_export_dest_type_t type,
                                                sbs_export_slot_t *slot)
{
    if (!ct || !slot) return;
    sbs_compositor_export_dest_release(&ct->compositor, type, slot);
}

bool sbs_compositor_thread_dest_export_available(sbs_compositor_thread_t *ct,
                                                   sbs_export_dest_type_t type)
{
    if (!ct) return false;
    return sbs_compositor_dest_export_available(&ct->compositor, type);
}

bool sbs_compositor_thread_native_canvas_enabled(sbs_compositor_thread_t *ct)
{
    return ct && ct->compositor.native_canvas.initialized;
}

bool sbs_compositor_thread_native_mailbox_acquire(sbs_compositor_thread_t *ct,
                                                   sbs_native_canvas_mailbox_type_t type,
                                                   sbs_native_canvas_lease_t *lease)
{
    uint32_t entry_idx = 0;
    uint64_t frame_number = 0;
    sbs_native_canvas_entry_t *entry = NULL;

    if (!ct || !lease)
        return false;

    if (!sbs_compositor_native_canvas_mailbox_acquire(&ct->compositor, type,
                                                      &entry_idx, &frame_number,
                                                      &entry))
        return false;

    if (!entry || entry->backing_fd < 0 || entry->backing_size == 0) {
        sbs_compositor_native_canvas_mailbox_release(&ct->compositor, entry_idx);
        return false;
    }

    memset(lease, 0, sizeof(*lease));
    lease->entry_idx = entry_idx;
    lease->frame_number = frame_number;
    lease->content_frame_number = entry->content_frame_number;
    lease->backing_fd = entry->backing_fd;
    lease->backing_size = (size_t)entry->backing_size;
    lease->host_mapped = entry->encoder_mapped ? entry->encoder_mapped
                                               : entry->backing_mapped;
    lease->host_size = entry->encoder_mapped ? (size_t)entry->encoder_size
                                             : (size_t)entry->backing_size;
    lease->host_is_backing = entry->encoder_mapped == NULL && entry->backing_mapped != NULL;
    lease->width = entry->y.width;
    lease->height = entry->y.height;
    lease->drm_modifier = entry->drm_modifier;
    lease->color_mode = entry->color_mode;
    lease->plane_offset[0] = (uint32_t)entry->y.offset;
    lease->plane_offset[1] = (uint32_t)entry->uv.offset;
    lease->plane_stride[0] = (uint32_t)entry->y.stride;
    lease->plane_stride[1] = (uint32_t)entry->uv.stride;
    if (entry->color_mode == SBS_EXPORT_COLOR_HDR10) {
        lease->drm_format = DRM_FORMAT_P010;
        lease->n_planes = 1;
    } else {
        lease->drm_format = DRM_FORMAT_NV21;
        lease->n_planes = 2;
    }
    return true;
}

void sbs_compositor_thread_native_mailbox_release(sbs_compositor_thread_t *ct,
                                                   const sbs_native_canvas_lease_t *lease)
{
    if (!ct || !lease)
        return;
    sbs_compositor_native_canvas_mailbox_release(&ct->compositor, lease->entry_idx);
}

void sbs_compositor_thread_native_mailbox_get_stats(sbs_compositor_thread_t *ct,
                                                     sbs_native_canvas_mailbox_type_t type,
                                                     sbs_native_canvas_mailbox_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!ct || type >= SBS_NATIVE_CANVAS_MAILBOX_COUNT ||
        !ct->compositor.native_canvas.mailboxes_initialized)
        return;

    sbs_native_canvas_mailbox_t *mailbox = &ct->compositor.native_canvas.mailboxes[type];
    pthread_mutex_lock(&mailbox->lock);
    out->has_entry = mailbox->has_entry;
    out->entry_idx = mailbox->entry_idx;
    out->frame_number = mailbox->frame_number;
    out->published = mailbox->published;
    out->dropped = mailbox->dropped;
    out->consumed = mailbox->consumed;
    out->late = mailbox->late;
    pthread_mutex_unlock(&mailbox->lock);
}

void sbs_compositor_thread_get_dimensions(sbs_compositor_thread_t *ct,
                                           uint32_t *width, uint32_t *height)
{
    if (!ct || !width || !height) return;
    *width = ct->width;
    *height = ct->height;
}

void sbs_compositor_thread_get_canvas_config(sbs_compositor_thread_t *ct,
                                             uint32_t *width,
                                             uint32_t *height,
                                             uint32_t *fps,
                                             sbs_export_color_mode_t *color_mode)
{
    if (!ct)
        return;
    if (width)
        *width = ct->width;
    if (height)
        *height = ct->height;
    if (fps)
        *fps = ct->fps;
    if (color_mode)
        *color_mode = ct->color_mode;
}
