/*
 * SBS - StreamBox Broadcast System
 * Export Destination — abstractions for destination-oriented GPU export
 *
 * Each export destination (preview, output) has its own resolution,
 * color mode, and ring of export slots. The compositor renders the
 * composed scene once, then each destination's export pass samples
 * the composed RGBA and produces the destination's encoder format at native
 * resolution: NV21 for SDR or P010 for HDR10.
 *
 * Export slots follow an explicit lifecycle:
 *   FREE → SUBMITTED → READY → IN_USE → FREE
 *
 * - FREE: slot can be written by the export pass
 * - SUBMITTED: GPU export work has been recorded and submitted
 * - READY: GPU work is complete, frame data is available for consumption
 * - IN_USE: a consumer (encoder/preview) is using this slot's data
 *
 * This replaces the previous synchronous fence-wait-per-frame approach
 * with readiness-based consumption.
 */
#ifndef SBS_EXPORT_DEST_H
#define SBS_EXPORT_DEST_H

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* ── Constants ────────────────────────────────────────────────── */

/** Number of export slots per destination ring.
 *  3 allows render to stay ahead while consumer processes the previous frame. */
#define SBS_EXPORT_SLOT_COUNT  8

/* ── Enums ────────────────────────────────────────────────────── */

/** Export destination type. */
typedef enum sbs_export_dest_type {
    SBS_EXPORT_DEST_PREVIEW = 0,   /**< WebRTC preview (latency-sensitive, lower res) */
    SBS_EXPORT_DEST_OUTPUT  = 1,   /**< Encoder output (throughput-sensitive, full res) */
    SBS_EXPORT_DEST_COUNT   = 2,
} sbs_export_dest_type_t;

/** Color mode for export conversion. */
typedef enum sbs_export_color_mode {
    SBS_EXPORT_COLOR_SDR    = 0,   /**< BT.601/709 SDR (NV21 output) */
    SBS_EXPORT_COLOR_HDR10  = 1,   /**< PQ HDR10 output (P010) */
} sbs_export_color_mode_t;

/** Export slot lifecycle state. */
typedef enum sbs_export_slot_state {
    SBS_EXPORT_SLOT_FREE      = 0, /**< Available for export pass to write */
    SBS_EXPORT_SLOT_SUBMITTED = 1, /**< GPU work submitted, not yet complete */
    SBS_EXPORT_SLOT_READY     = 2, /**< GPU complete, data available for consumer */
    SBS_EXPORT_SLOT_IN_USE    = 3, /**< Consumer is reading this slot's data */
} sbs_export_slot_state_t;

/* ── Export Slot ──────────────────────────────────────────────── */

/** A single export slot in the ring.
 *
 *  Each slot owns GPU resources for one frame export:
 *  - A render target image (at destination resolution) for the export pass
 *  - A DMA-BUF-backed output buffer (NV21 for SDR, P010 for HDR10)
 *  - A fence for tracking GPU completion
 *  - A command buffer for the export pass
 */
typedef struct sbs_export_slot {
    /* GPU resources */
    VkImage         render_image;     /**< Destination-sized image for export pass */
    VkDeviceMemory  render_memory;
    VkImageView     render_view;
    VkFramebuffer   framebuffer;      /**< Framebuffer for export render pass */

    VkBuffer        nv21_buffer;      /**< Output buffer; legacy name, stores NV21 or P010 */
    VkDeviceMemory  nv21_memory;
    void           *nv21_mapped;      /**< Persistently mapped pointer */
    VkDeviceSize    nv21_size;        /**< Output byte size for NV21 or P010 */
    int             nv21_dmabuf_fd;   /**< Exported DMABUF fd for GstMemory ingest */

    VkFence         fence;            /**< Signaled when export GPU work completes */
    VkCommandBuffer cmd_buffer;

    /* Descriptor set for this slot's export pass */
    VkDescriptorSet descriptor_set;

    /* Subpass1 zero-copy alias: when the tile-fused render pass already
     * produced NV21 at canvas resolution, nv21_mapped is swapped to point
     * at the render target's NV21 buffer directly (no memcpy, no GPU compute).
     * nv21_mapped_own stores the slot's original buffer pointer for restore. */
    void           *nv21_mapped_own;  /**< Slot's own buffer (saved during alias) */
    VkDeviceSize    nv21_size_own;    /**< Slot's own buffer size (saved during alias) */
    bool            subpass1_alias;   /**< true if nv21_mapped is aliased */

    /* Lifecycle */
    _Atomic int     state;            /**< sbs_export_slot_state_t */
    uint64_t        frame_number;     /**< Frame sequence number */
    uint64_t        submit_time_us;   /**< Monotonic timestamp of GPU submit */
    uint64_t        ready_time_us;    /**< Monotonic timestamp of ready transition */
    uint32_t        render_target_idx;/**< Source render target used for this export */
    _Atomic uint32_t retire_refs;     /**< Render targets still waiting on this fence */
    bool            release_pending_free; /**< Consumer released; free once retire_refs hits 0 */
} sbs_export_slot_t;

/* ── Export Destination Descriptor ────────────────────────────── */

/** Describes an export destination's requirements. */
typedef struct sbs_export_dest {
    sbs_export_dest_type_t   type;
    uint32_t                 width;        /**< Target width (e.g. 1920, 640) */
    uint32_t                 height;       /**< Target height (e.g. 1080, 360) */
    sbs_export_color_mode_t  color_mode;
    uint32_t                 framerate;    /**< Target framerate (0 = match source) */

    /* Destination-specific export pass resources */
    VkRenderPass             export_render_pass;
    VkPipelineLayout         export_pipeline_layout;
    VkPipeline               export_pipeline;
    VkDescriptorSetLayout    export_ds_layout;
    VkDescriptorPool         export_ds_pool;

    /* Sampler for reading the composed RGBA render target */
    VkSampler                source_sampler;

    /* Export slot ring */
    sbs_export_slot_t        slots[SBS_EXPORT_SLOT_COUNT];
    uint32_t                 submit_idx;   /**< Next slot to submit to */
    uint32_t                 consume_idx;  /**< Next slot to consume from */

    /* State */
    bool                     initialized;
    bool                     active;       /**< Consumer is connected */

    /* Diagnostics */
    struct {
        _Atomic uint64_t     frames_exported;     /**< Total frames exported */
        _Atomic uint64_t     frames_dropped;      /**< Frames dropped (ring full) */
        _Atomic uint64_t     frames_consumed;     /**< Frames consumed by downstream */
        _Atomic uint64_t     slots_recycled;      /**< Total slot recycles */
        _Atomic uint64_t     init_failures;       /**< Init failure count */
        _Atomic uint64_t     total_export_time_us; /**< Cumulative GPU export time */
        _Atomic uint64_t     total_wait_time_us;   /**< Cumulative fence wait time */
        uint64_t             last_export_time_us;  /**< Most recent export time */
        uint64_t             last_wait_time_us;    /**< Most recent fence wait time */
        uint32_t             current_backlog;      /**< Slots in SUBMITTED state */
    } diag;
} sbs_export_dest_t;

/* ── Slot Ring Operations ────────────────────────────────────── */

/**
 * Initialize an export destination with the given parameters.
 * Allocates GPU resources for all slots in the ring.
 *
 * @param dest       Destination to initialize
 * @param device     Vulkan device
 * @param mem_props  Device memory properties
 * @param type       Destination type
 * @param width      Target width
 * @param height     Target height
 * @param color_mode Color conversion mode
 * @return 0 on success, -1 on failure (hard error)
 */
int sbs_export_dest_init_slots(sbs_export_dest_t *dest,
                               VkDevice device,
                               const VkPhysicalDeviceMemoryProperties *mem_props,
                               VkCommandPool cmd_pool);

int sbs_export_dest_init(sbs_export_dest_t *dest,
                         VkDevice device,
                         const VkPhysicalDeviceMemoryProperties *mem_props,
                         sbs_export_dest_type_t type,
                         uint32_t width, uint32_t height,
                         sbs_export_color_mode_t color_mode);

/**
 * Destroy an export destination and free all GPU resources.
 */
void sbs_export_dest_destroy(sbs_export_dest_t *dest, VkDevice device);

/**
 * Acquire the next free slot for GPU export submission.
 * Returns NULL if all slots are busy (ring full — frame will be dropped).
 */
sbs_export_slot_t *sbs_export_dest_acquire_slot(sbs_export_dest_t *dest);

/**
 * Mark a slot as submitted (GPU work recorded and submitted).
 */
void sbs_export_slot_mark_submitted(sbs_export_slot_t *slot, uint64_t frame_number);

/**
 * Poll a submitted slot's fence. If complete, transitions to READY.
 * @return true if the slot is now READY, false if still in-flight.
 */
bool sbs_export_slot_poll_ready(sbs_export_slot_t *slot, VkDevice device);

/**
 * Acquire the next ready slot for consumer use.
 * Transitions the slot from READY → IN_USE.
 * Returns NULL if no slot is ready yet.
 */
sbs_export_slot_t *sbs_export_dest_acquire_ready(sbs_export_dest_t *dest, VkDevice device);

/**
 * Release a slot back to the ring after consumer is done.
 * Transitions IN_USE → FREE and resets the fence.
 */
void sbs_export_slot_release(sbs_export_slot_t *slot, VkDevice device);

/**
 * Log diagnostic summary for this destination.
 */
void sbs_export_dest_log_diag(const sbs_export_dest_t *dest);

#endif /* SBS_EXPORT_DEST_H */
