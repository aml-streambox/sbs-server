/*
 * SBS - StreamBox Broadcast System
 * Export Destination — slot ring lifecycle and GPU resource management
 */
#define SBS_LOG_COMP "export-dest"

#include "sbs/export_dest.h"
#include "sbs/dmabuf_alloc.h"
#include "sbs/log.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────── */

static uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties *props,
                                 uint32_t type_bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

static const char *dest_type_str(sbs_export_dest_type_t type)
{
    switch (type) {
    case SBS_EXPORT_DEST_PREVIEW: return "preview";
    case SBS_EXPORT_DEST_OUTPUT:  return "output";
    default:                      return "unknown";
    }
}

static const char *slot_state_str(int state)
{
    switch (state) {
    case SBS_EXPORT_SLOT_FREE:      return "FREE";
    case SBS_EXPORT_SLOT_SUBMITTED: return "SUBMITTED";
    case SBS_EXPORT_SLOT_READY:     return "READY";
    case SBS_EXPORT_SLOT_IN_USE:    return "IN_USE";
    default:                        return "?";
    }
}

/* ── Slot Ring Init/Destroy ──────────────────────────────────── */

static int init_slot(sbs_export_slot_t *slot, VkDevice device,
                     const VkPhysicalDeviceMemoryProperties *mem_props,
                     uint32_t width, uint32_t height,
                     sbs_export_color_mode_t color_mode,
                     VkCommandPool cmd_pool)
{
    VkResult res;
    memset(slot, 0, sizeof(*slot));
    slot->nv21_dmabuf_fd = -1;
    atomic_store(&slot->state, SBS_EXPORT_SLOT_FREE);
    atomic_store(&slot->retire_refs, 0);

    VkDeviceSize nv21_size;
    if (color_mode == SBS_EXPORT_COLOR_HDR10) {
        nv21_size = (VkDeviceSize)width * height * 4;
    } else {
        nv21_size = (VkDeviceSize)width * height * 3 / 2;
    }
    slot->nv21_size = nv21_size;

    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = nv21_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkExternalMemoryBufferCreateInfo ext_buf_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    buf_ci.pNext = &ext_buf_ci;
    res = vkCreateBuffer(device, &buf_ci, NULL, &slot->nv21_buffer);
    if (res != VK_SUCCESS) {
        LOG_E("failed to create NV21 buffer: %d", res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device, slot->nv21_buffer, &mem_req);

    uint32_t mem_type = find_memory_type(mem_props, mem_req.memoryTypeBits, 0);
    if (mem_type == UINT32_MAX) {
        LOG_E("no compatible memory type for export slot NV21 buffer");
        return -1;
    }

    sbs_dmabuf_alloc_t alloc;
    sbs_dmabuf_buffer_t backing;
    if (sbs_dmabuf_alloc_open(&alloc) != SBS_OK || !alloc.available) {
        LOG_E("no DMA-BUF allocator available for export slot");
        sbs_dmabuf_alloc_close(&alloc);
        return -1;
    }
    if (sbs_dmabuf_alloc_buffer(&alloc, nv21_size, 0, &backing) != SBS_OK || backing.fd < 0) {
        LOG_E("failed to allocate DMA-BUF backing for export slot");
        sbs_dmabuf_alloc_close(&alloc);
        return -1;
    }
    sbs_dmabuf_alloc_close(&alloc);
    slot->nv21_dmabuf_fd = backing.fd;

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup(slot->nv21_dmabuf_fd),
    };
    if (import_info.fd < 0) {
        LOG_E("dup failed for export slot DMA-BUF");
        return -1;
    }
    alloc_info.pNext = &import_info;
    res = vkAllocateMemory(device, &alloc_info, NULL, &slot->nv21_memory);
    if (res != VK_SUCCESS) {
        LOG_E("failed to allocate NV21 buffer memory: %d", res);
        return -1;
    }
    vkBindBufferMemory(device, slot->nv21_buffer, slot->nv21_memory, 0);

    /* CPU mapping is for legacy paths only; encoder/preview use the DMABUF fd. */
    slot->nv21_mapped = mmap(NULL, nv21_size, PROT_READ | PROT_WRITE, MAP_SHARED, slot->nv21_dmabuf_fd, 0);
    if (slot->nv21_mapped == MAP_FAILED) {
        slot->nv21_mapped = NULL;
        LOG_W("failed to mmap export slot DMA-BUF; legacy CPU consumers disabled");
    }

    if (slot->nv21_dmabuf_fd < 0) {
        LOG_E("invalid NV21 DMA-BUF fd after allocation");
        return -1;
    }

    /* Fence (starts signaled so first acquire works) */
    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    res = vkCreateFence(device, &fence_ci, NULL, &slot->fence);
    if (res != VK_SUCCESS) {
        LOG_E("failed to create export slot fence: %d", res);
        return -1;
    }

    /* Command buffer */
    VkCommandBufferAllocateInfo cb_ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(device, &cb_ai, &slot->cmd_buffer);
    if (res != VK_SUCCESS) {
        LOG_E("failed to allocate export slot command buffer: %d", res);
        return -1;
    }

    return 0;
}

static void destroy_slot(sbs_export_slot_t *slot, VkDevice device)
{
    if (!slot) return;
    /* Note: command buffers are freed when the pool is destroyed */
    if (slot->fence)       vkDestroyFence(device, slot->fence, NULL);
    if (slot->nv21_mapped) munmap(slot->nv21_mapped, slot->nv21_size);
    if (slot->nv21_dmabuf_fd >= 0) close(slot->nv21_dmabuf_fd);
    if (slot->nv21_buffer) vkDestroyBuffer(device, slot->nv21_buffer, NULL);
    if (slot->nv21_memory) vkFreeMemory(device, slot->nv21_memory, NULL);
    if (slot->render_view) vkDestroyImageView(device, slot->render_view, NULL);
    if (slot->framebuffer) vkDestroyFramebuffer(device, slot->framebuffer, NULL);
    if (slot->render_image) vkDestroyImage(device, slot->render_image, NULL);
    if (slot->render_memory) vkFreeMemory(device, slot->render_memory, NULL);
    memset(slot, 0, sizeof(*slot));
}

/* ── Destination Init/Destroy ────────────────────────────────── */

int sbs_export_dest_init(sbs_export_dest_t *dest,
                         VkDevice device,
                         const VkPhysicalDeviceMemoryProperties *mem_props,
                         sbs_export_dest_type_t type,
                         uint32_t width, uint32_t height,
                         sbs_export_color_mode_t color_mode)
{
    memset(dest, 0, sizeof(*dest));
    dest->type       = type;
    dest->width      = width;
    dest->height     = height;
    dest->color_mode = color_mode;

    LOG_I("initializing %s export destination: %ux%u color=%d",
          dest_type_str(type), width, height, color_mode);

    /* NOTE: GPU pipeline resources (render pass, pipeline, descriptor sets)
     * and per-slot render targets are set up by the compositor after calling
     * this function, since they depend on the compositor's command pool,
     * render pass configuration, and shader modules.
     *
     * This function only allocates the NV21 output buffers, fences, and
     * command buffers that are independent of the export pipeline. The
     * command pool must be passed in separately. */

    dest->submit_idx  = 0;
    dest->consume_idx = 0;
    dest->initialized = true;

    LOG_I("%s export destination initialized: %ux%u, %d slots",
          dest_type_str(type), width, height, SBS_EXPORT_SLOT_COUNT);
    return 0;
}

/**
 * Initialize the slots in a destination ring.
 * Called after sbs_export_dest_init() when command pool is available.
 */
int sbs_export_dest_init_slots(sbs_export_dest_t *dest,
                                VkDevice device,
                                const VkPhysicalDeviceMemoryProperties *mem_props,
                                VkCommandPool cmd_pool)
{
    for (int i = 0; i < SBS_EXPORT_SLOT_COUNT; i++) {
        if (init_slot(&dest->slots[i], device, mem_props,
                      dest->width, dest->height, dest->color_mode, cmd_pool) != 0) {
            LOG_E("failed to init %s export slot %d", dest_type_str(dest->type), i);
            atomic_fetch_add(&dest->diag.init_failures, 1);
            /* Clean up previously initialized slots */
            for (int j = 0; j < i; j++)
                destroy_slot(&dest->slots[j], device);
            dest->initialized = false;
            return -1;
        }
    }
    return 0;
}

void sbs_export_dest_destroy(sbs_export_dest_t *dest, VkDevice device)
{
    if (!dest || !dest->initialized) return;

    LOG_I("destroying %s export destination", dest_type_str(dest->type));

    for (int i = 0; i < SBS_EXPORT_SLOT_COUNT; i++)
        destroy_slot(&dest->slots[i], device);

    if (dest->export_pipeline)
        vkDestroyPipeline(device, dest->export_pipeline, NULL);
    if (dest->export_pipeline_layout)
        vkDestroyPipelineLayout(device, dest->export_pipeline_layout, NULL);
    if (dest->export_render_pass)
        vkDestroyRenderPass(device, dest->export_render_pass, NULL);
    if (dest->export_ds_layout)
        vkDestroyDescriptorSetLayout(device, dest->export_ds_layout, NULL);
    if (dest->export_ds_pool)
        vkDestroyDescriptorPool(device, dest->export_ds_pool, NULL);
    if (dest->source_sampler)
        vkDestroySampler(device, dest->source_sampler, NULL);

    sbs_export_dest_log_diag(dest);
    memset(dest, 0, sizeof(*dest));
}

/* ── Slot Ring Operations ────────────────────────────────────── */

sbs_export_slot_t *sbs_export_dest_acquire_slot(sbs_export_dest_t *dest)
{
    /* Try each slot starting from submit_idx */
    for (uint32_t i = 0; i < SBS_EXPORT_SLOT_COUNT; i++) {
        uint32_t idx = (dest->submit_idx + i) % SBS_EXPORT_SLOT_COUNT;
        sbs_export_slot_t *slot = &dest->slots[idx];

        int expected = SBS_EXPORT_SLOT_FREE;
        if (atomic_compare_exchange_strong(&slot->state, &expected,
                                           SBS_EXPORT_SLOT_IN_USE)) {
            /* Slot is free — claim it */
            dest->submit_idx = (idx + 1) % SBS_EXPORT_SLOT_COUNT;
            return slot;
        }
    }

    /* Ring full — log slot states for debugging */
    {
        static uint64_t last_log = 0;
        uint64_t now = g_get_monotonic_time();
        if (now - last_log > 1000000) { /* log at most once per second */
            last_log = now;
            char states[32] = {0};
            for (uint32_t j = 0; j < SBS_EXPORT_SLOT_COUNT && j < sizeof(states)-1; j++) {
                int st = atomic_load(&dest->slots[j].state);
                states[j] = "FSRI?"[st < 5 ? st : 4];
            }
            LOG_E("RING FULL %s: slots=%s dropped=%lu",
                  dest_type_str(dest->type), states,
                  (unsigned long)atomic_load(&dest->diag.frames_dropped));
        }
    }
    atomic_fetch_add(&dest->diag.frames_dropped, 1);
    dest->diag.current_backlog = SBS_EXPORT_SLOT_COUNT;
    return NULL;
}

void sbs_export_slot_mark_submitted(sbs_export_slot_t *slot, uint64_t frame_number)
{
    slot->frame_number = frame_number;
    slot->submit_time_us = 0; /* caller should set this */
    slot->ready_time_us = 0;
    slot->release_pending_free = false;
    atomic_store(&slot->state, SBS_EXPORT_SLOT_SUBMITTED);
}

bool sbs_export_slot_poll_ready(sbs_export_slot_t *slot, VkDevice device)
{
    int st = atomic_load(&slot->state);
    if (st != SBS_EXPORT_SLOT_SUBMITTED) return (st == SBS_EXPORT_SLOT_READY);

    VkResult res = vkGetFenceStatus(device, slot->fence);
    if (res == VK_SUCCESS) {
        slot->ready_time_us = 0; /* caller should set */
        atomic_store(&slot->state, SBS_EXPORT_SLOT_READY);
        return true;
    }
    return false;
}

sbs_export_slot_t *sbs_export_dest_acquire_ready(sbs_export_dest_t *dest, VkDevice device)
{
    /* First poll all submitted slots to see if any are ready */
    uint32_t backlog = 0;
    for (uint32_t i = 0; i < SBS_EXPORT_SLOT_COUNT; i++) {
        sbs_export_slot_t *slot = &dest->slots[i];
        int st = atomic_load(&slot->state);
        if (st == SBS_EXPORT_SLOT_SUBMITTED) {
            sbs_export_slot_poll_ready(slot, device);
            st = atomic_load(&slot->state);
            if (st == SBS_EXPORT_SLOT_SUBMITTED)
                backlog++;
        }
    }
    dest->diag.current_backlog = backlog;

    /* Find the oldest ready slot (lowest frame_number) */
    sbs_export_slot_t *best = NULL;
    for (uint32_t i = 0; i < SBS_EXPORT_SLOT_COUNT; i++) {
        sbs_export_slot_t *slot = &dest->slots[i];
        if (atomic_load(&slot->state) == SBS_EXPORT_SLOT_READY) {
            if (!best || slot->frame_number < best->frame_number)
                best = slot;
        }
    }

    if (best) {
        atomic_store(&best->state, SBS_EXPORT_SLOT_IN_USE);
        atomic_fetch_add(&dest->diag.frames_consumed, 1);
    }
    return best;
}

void sbs_export_slot_release(sbs_export_slot_t *slot, VkDevice device)
{
    if (!slot) return;
    (void)device;
    /* Restore aliased pointer if subpass1 zero-copy was used */
    if (slot->subpass1_alias) {
        slot->nv21_mapped = slot->nv21_mapped_own;
        slot->nv21_mapped_own = NULL;
        if (slot->nv21_size_own) {
            slot->nv21_size = slot->nv21_size_own;
            slot->nv21_size_own = 0;
        }
        slot->subpass1_alias = false;
    }
    if (atomic_load(&slot->retire_refs) == 0) {
        atomic_store(&slot->state, SBS_EXPORT_SLOT_FREE);
    } else {
        slot->release_pending_free = true;
        atomic_store(&slot->state, SBS_EXPORT_SLOT_IN_USE);
    }
}

/* ── Diagnostics ─────────────────────────────────────────────── */

void sbs_export_dest_log_diag(const sbs_export_dest_t *dest)
{
    if (!dest) return;

    uint64_t exported = atomic_load(&dest->diag.frames_exported);
    uint64_t dropped  = atomic_load(&dest->diag.frames_dropped);
    uint64_t consumed = atomic_load(&dest->diag.frames_consumed);
    uint64_t recycled = atomic_load(&dest->diag.slots_recycled);
    uint64_t failures = atomic_load(&dest->diag.init_failures);

    LOG_I("DIAG %s: exported=%lu dropped=%lu consumed=%lu recycled=%lu "
          "init_failures=%lu backlog=%u",
          dest_type_str(dest->type),
          (unsigned long)exported, (unsigned long)dropped,
          (unsigned long)consumed, (unsigned long)recycled,
          (unsigned long)failures, dest->diag.current_backlog);

    if (exported > 0) {
        uint64_t total_export = atomic_load(&dest->diag.total_export_time_us);
        uint64_t total_wait   = atomic_load(&dest->diag.total_wait_time_us);
        LOG_I("DIAG %s: avg_export=%.1fms avg_wait=%.1fms last_export=%.1fms last_wait=%.1fms",
              dest_type_str(dest->type),
              (double)total_export / exported / 1000.0,
              (double)total_wait / exported / 1000.0,
              dest->diag.last_export_time_us / 1000.0,
              dest->diag.last_wait_time_us / 1000.0);
    }
}
