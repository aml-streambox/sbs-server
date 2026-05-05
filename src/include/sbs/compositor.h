#ifndef SBS_COMPOSITOR_H
#define SBS_COMPOSITOR_H

#include "sbs/types.h"
#include "sbs/compositor_scene.h"
#include "sbs/export_dest.h"
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>

#define SBS_RENDER_TARGET_COUNT 8
#define SBS_NATIVE_CANVAS_RING_SIZE 4
#define SBS_PUSH_CONSTANT_SIZE 176  /* 160 original + 16 bytes for has_texture + padding */
#define SBS_MAX_SOURCE_TEXTURES SBS_COMP_SCENE_MAX_ITEMS
#define SBS_NATIVE_LAYER_DESCRIPTOR_SETS (SBS_COMP_SCENE_MAX_ITEMS * 2u)
#define SBS_NATIVE_TIMING_MAX_LAYERS 8
#define SBS_NATIVE_TIMING_LABEL_MAX 64

typedef enum sbs_native_canvas_entry_state {
    SBS_NATIVE_CANVAS_ENTRY_FREE = 0,
    SBS_NATIVE_CANVAS_ENTRY_RENDERING = 1,
    SBS_NATIVE_CANVAS_ENTRY_READY = 2,
} sbs_native_canvas_entry_state_t;

typedef enum sbs_native_canvas_mailbox_type {
    SBS_NATIVE_CANVAS_MAILBOX_PREVIEW = 0,
    SBS_NATIVE_CANVAS_MAILBOX_OUTPUT = 1,
    SBS_NATIVE_CANVAS_MAILBOX_SNAPSHOT = 2,
    SBS_NATIVE_CANVAS_MAILBOX_COUNT = 3,
} sbs_native_canvas_mailbox_type_t;

typedef struct sbs_native_canvas_plane {
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageView     view;
    VkImageLayout   layout;
    VkFormat        format;
    uint32_t        width;
    uint32_t        height;
    VkDeviceSize    stride;
    VkDeviceSize    offset;
} sbs_native_canvas_plane_t;

typedef struct sbs_native_canvas_entry {
    sbs_native_canvas_plane_t y;
    sbs_native_canvas_plane_t uv;

    int             backing_fd;
    VkDeviceSize    backing_size;
    void           *backing_mapped;
    uint64_t        drm_modifier;
    sbs_export_color_mode_t color_mode;

    VkBuffer        encoder_buffer;
    VkDeviceMemory  encoder_memory;
    void           *encoder_mapped;
    VkDeviceSize    encoder_size;

    VkFence         fence;
    VkCommandBuffer cmd_buffer;
    _Atomic uint32_t refcount;
    _Atomic int     state;
    uint64_t        frame_number;
    uint64_t        content_frame_number;
    uint64_t        submit_time_us;
    uint32_t        timing_query_base;
    uint32_t        timing_mark_count;
    uint32_t        timing_layer_count;
    char            timing_layer_labels[SBS_NATIVE_TIMING_MAX_LAYERS][SBS_NATIVE_TIMING_LABEL_MAX];
    bool            timing_query_valid;
    bool            allocated;
} sbs_native_canvas_entry_t;

typedef struct sbs_native_canvas_mailbox_entry {
    uint32_t entry_idx;
    uint64_t frame_number;
} sbs_native_canvas_mailbox_entry_t;

typedef struct sbs_native_canvas_mailbox {
    pthread_mutex_t lock;
    bool            has_entry;
    uint32_t        entry_idx;
    uint64_t        frame_number;
    sbs_native_canvas_mailbox_entry_t entries[SBS_NATIVE_CANVAS_RING_SIZE];
    uint32_t        read_idx;
    uint32_t        write_idx;
    uint32_t        count;
    uint64_t        published;
    uint64_t        dropped;
    uint64_t        consumed;
    uint64_t        late;
} sbs_native_canvas_mailbox_t;

/* Source texture slot — one per unique live source used by scene items.
 * Each slot has a device-local RGBA8 image used as the intermediate target
 * for GPU DMA-BUF YUV->RGBA conversion before composition. */
typedef struct sbs_source_texture {
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageView     view;
    VkBuffer        staging_buf;
    VkDeviceMemory  staging_mem;
    void           *staging_mapped;   /* persistently mapped pointer */
    VkDeviceSize    staging_size;
    uint32_t        width;
    uint32_t        height;
    char            source_id[128];
    bool            allocated;
    bool            has_content;      /* true after first upload */
    bool            upload_pending;   /* OPT-3: deferred upload flag */
    uint64_t        last_frame_ts;    /* frame_slot timestamp of last uploaded frame */

    /* DMA-BUF buffer import cache — pool for YUV buffer imports */
#define SBS_DMABUF_BUF_CACHE_SIZE 16
    struct sbs_dmabuf_buf_cache_entry {
        VkBuffer        buf;
        VkImage         ycbcr_image;    /* NV12 VkImage for ycbcr sampling (replaces buf path) */
        VkImageView     ycbcr_view;     /* view with ycbcr conversion attached */
        VkDeviceMemory  memory;
        ino_t           inode;
        uint32_t        width;
        uint32_t        height;
        uint32_t        drm_format;
        uint32_t        y_stride;
        uint32_t        uv_stride;
        uint32_t        uv_offset;
        uint32_t        alpha_x;
        uint32_t        alpha_y;
        uint32_t        alpha_w;
        uint32_t        alpha_h;
        bool            alpha_rect_opaque;
        uint32_t        age;
        bool            valid;
        bool            is_ycbcr;       /* true if this entry uses ycbcr image path */
    } dmabuf_buf_cache[SBS_DMABUF_BUF_CACHE_SIZE];
    int              dmabuf_buf_active_idx;
    uint32_t         dmabuf_buf_frame_counter;

    /* Legacy single-entry fields kept for API compat */
    bool            dmabuf_imported;
    VkDeviceMemory  dmabuf_memory;
    VkBuffer        y_buf;
    VkBuffer        uv_buf;
    uint32_t        y_stride;
    uint32_t        uv_stride;
    uint32_t        uv_offset;
    uint32_t        alpha_x;
    uint32_t        alpha_y;
    uint32_t        alpha_w;
    uint32_t        alpha_h;
    bool            alpha_rect_opaque;
    uint32_t        drm_format;
    uint32_t        upload_filter_flags;

    /* DMA-BUF image import cache — pool of imports keyed by inode.
     * V4L2 reuses a fixed set of buffers; same inode = same backing store,
     * so we skip all Vulkan object creation on cache hit. */
#define SBS_DMABUF_IMAGE_CACHE_SIZE 8
    struct sbs_dmabuf_image_cache_entry {
        VkImage         image;
        VkDeviceMemory  memory;
        VkImageView     view;
        VkImageLayout   layout;
        ino_t           inode;       /* DMA-BUF identity */
        uint32_t        width;
        uint32_t        height;
        uint64_t        modifier;
        uint32_t        age;         /* frame counter for LRU eviction */
        bool            valid;
    } dmabuf_image_cache[SBS_DMABUF_IMAGE_CACHE_SIZE];
    int              dmabuf_image_active_idx;  /* currently bound cache entry, or -1 */
    uint32_t         dmabuf_image_frame_counter;

    /* Legacy single-entry fields kept for API compat with render loop */
    bool            dmabuf_image_imported;
    VkImage         dmabuf_image;
    VkImageView     dmabuf_image_view;
    VkImageLayout   dmabuf_image_layout;
    uint64_t        dmabuf_modifier;

    /* Currently bound graphics descriptor view for this slot. */
    VkImageView     descriptor_view;

    /* YCbCr direct sampling — when true, composition uses ycbcr pipeline */
    bool            ycbcr_imported;
    VkImageView     ycbcr_active_view;  /* the NV12 ycbcr image view currently bound */

    /* Optional per-item custom 3D LUT for compositor filters. */
    VkImage         lut_image;
    VkDeviceMemory  lut_memory;
    VkImageView     lut_view;
    VkImageView     lut_descriptor_view;
    VkImageView     ycbcr_lut_descriptor_view;
    uint32_t        lut_size;
    char            lut_path[SBS_COMP_LUT_PATH_MAX];
} sbs_source_texture_t;

typedef struct sbs_export_surface {
    VkImage         image;
    VkDeviceMemory  memory;
    void           *mapped;
    VkDeviceSize    size;
    VkDeviceSize    offset;
    VkDeviceSize    row_pitch;
    VkImageLayout   layout;
    uint32_t        width;
    uint32_t        height;
} sbs_export_surface_t;

typedef struct sbs_blit_target {
    VkImage         image;
    VkDeviceMemory  memory;
    VkImageLayout   layout;
    uint32_t        width;
    uint32_t        height;
} sbs_blit_target_t;

typedef struct sbs_compositor {
    VkInstance              instance;
    VkPhysicalDevice        physical_device;
    VkDevice                device;
    uint32_t                queue_family;
    VkQueue                 graphics_queue;
    pthread_mutex_t         queue_mutex;    /* protects graphics_queue submissions */
    VkCommandPool           command_pool;
    pthread_mutex_t         command_pool_mutex; /* protects command_pool alloc/free/reset ops */
    pthread_mutex_t         retire_slot_mutex;  /* protects targets[i].retire_slot read/write/clear */
    VkPhysicalDeviceMemoryProperties mem_props;
    float                   timestamp_period_ns;
    uint32_t                timestamp_valid_bits;
    VkQueryPool             native_timing_query_pool;
    bool                    native_timing_available;
    bool                    native_timing_detail;

    struct {
        VkImage         image;
        VkDeviceMemory  memory;
        VkImageView     view;
        VkFramebuffer   framebuffer;
        VkFence         fence;
        VkCommandBuffer cmd_buffer;
        int             dmabuf_fd;
        VkFormat        format;
        VkImageLayout   layout;     /* current image layout for transaction elimination */
        sbs_export_slot_t *retire_slot; /* latest export using this render target */
    } targets[SBS_RENDER_TARGET_COUNT];

    /* Staging buffer for frame export (LINEAR tiling, HOST_VISIBLE memory).
     * Used to copy rendered frames out when DMA-BUF export is unavailable. */
    sbs_export_surface_t    staging;
    sbs_blit_target_t       preview_target;
    sbs_export_surface_t    preview_staging;

    VkRenderPass            render_pass;
    VkPipelineLayout        pipeline_layout;
    VkPipeline              pipeline;
    VkPipelineCache         pipeline_cache;   /* OPT-8: persistent pipeline cache for Mali G52 */
    VkDescriptorSetLayout   descriptor_set_layout;
    VkDescriptorPool        descriptor_pool;
    VkDescriptorSet         descriptor_sets[SBS_MAX_SOURCE_TEXTURES];

    /* Source texture slots — one per potential scene item */
    sbs_source_texture_t    sources[SBS_MAX_SOURCE_TEXTURES];
    VkSampler               source_sampler;  /* shared sampler for all source textures */
    uint32_t                source_count;

    /* Built-in HDR10/PQ to SDR 3D LUT, also used as a valid fallback binding. */
    VkImage                 hdr_lut_image;
    VkDeviceMemory          hdr_lut_memory;
    VkImageView             hdr_lut_view;
    uint32_t                hdr_lut_size;

    /* Built-in HDR10/PQ to SDR YCbCr 3D LUT for direct YUV compute paths. */
    VkImage                 hdr_ycbcr_lut_image;
    VkDeviceMemory          hdr_ycbcr_lut_memory;
    VkImageView             hdr_ycbcr_lut_view;
    VkSampler               hdr_ycbcr_lut_sampler;
    uint32_t                hdr_ycbcr_lut_size;

    /* Compute pipeline for GPU-based YUV->RGBA conversion */
    VkShaderModule          compute_shader;
    VkPipelineLayout        compute_pipeline_layout;
    VkPipeline              compute_pipeline;
    VkDescriptorSetLayout   compute_ds_layout;
    VkDescriptorPool        compute_descriptor_pool;
    VkDescriptorSet         compute_descriptor_sets[SBS_MAX_SOURCE_TEXTURES];

    /* ── YCbCr direct sampling (VK_KHR_sampler_ycbcr_conversion) ── */
    /* Replaces compute YUV→RGBA for NV12 sources: GPU texture unit
     * converts NV12→RGB during sampling, zero compute overhead. */
    VkSamplerYcbcrConversion ycbcr_conversion;
    VkSampler               ycbcr_sampler;       /* immutable sampler with ycbcr conversion */
    VkDescriptorSetLayout   ycbcr_ds_layout;     /* layout with immutable ycbcr sampler */
    VkPipelineLayout        ycbcr_pipeline_layout;
    VkPipeline              ycbcr_pipeline;       /* same shaders, ycbcr descriptor layout */
    VkDescriptorPool        ycbcr_descriptor_pool;
    VkDescriptorSet         ycbcr_descriptor_sets[SBS_MAX_SOURCE_TEXTURES];
    bool                    ycbcr_available;

    /* Placeholder 1x1 magenta texture for items without live source data */
    sbs_source_texture_t    placeholder;

    uint32_t                width;
    uint32_t                height;
    _Atomic uint32_t        current_target;
    _Atomic uint32_t        last_rendered_target; /* set by compositor thread after render */
    bool                    initialized;
    bool                    dmabuf_export_available;

    /* Native YUV canvas capability gate.  The native path renders into
     * explicit Y/UV plane targets and imports externally allocated DMA-BUFs. */
    struct {
        bool         available;
        uint64_t     drm_modifier;
        VkFormat     sdr_y_format;
        VkFormat     sdr_uv_format;
        VkFormat     hdr_y_format;
        VkFormat     hdr_uv_format;
        VkDeviceSize sdr_y_stride;
        VkDeviceSize sdr_uv_stride;
        VkDeviceSize sdr_uv_offset;
        VkDeviceSize sdr_total_size;
        VkDeviceSize hdr_y_stride;
        VkDeviceSize hdr_uv_stride;
        VkDeviceSize hdr_uv_offset;
        VkDeviceSize hdr_total_size;
    } native_yuv;

    struct {
        bool                       initialized;
        bool                       mailboxes_initialized;
        uint32_t                   write_idx;
        _Atomic uint32_t           current_entry;
        _Atomic uint32_t           last_rendered_entry;
        sbs_native_canvas_entry_t  entries[SBS_NATIVE_CANVAS_RING_SIZE];
        sbs_native_canvas_mailbox_t mailboxes[SBS_NATIVE_CANVAS_MAILBOX_COUNT];
    } native_canvas;

    struct {
        bool                       initialized;
        uint32_t                   width;
        uint32_t                   height;
        uint32_t                   frame_interval;
        sbs_export_color_mode_t    color_mode;
        uint32_t                   write_idx;
        _Atomic uint32_t           last_rendered_entry;
        sbs_native_canvas_entry_t  entries[SBS_NATIVE_CANVAS_RING_SIZE];
    } native_preview;

    VkShaderModule          native_yuv_sdr_shader;
    VkShaderModule          native_yuv_hdr_shader;
    VkPipelineLayout        native_yuv_pipeline_layout;
    VkPipeline              native_yuv_sdr_pipeline;
    VkPipeline              native_yuv_hdr_pipeline;
    VkDescriptorSetLayout   native_yuv_ds_layout;
    VkDescriptorPool        native_yuv_ds_pool;
    VkDescriptorSet         native_yuv_ds[SBS_NATIVE_CANVAS_RING_SIZE][SBS_NATIVE_LAYER_DESCRIPTOR_SETS];
    VkDescriptorSet         native_yuv_preview_ds[SBS_NATIVE_CANVAS_RING_SIZE][SBS_NATIVE_LAYER_DESCRIPTOR_SETS];

    /* P010 direct native path: source P010 DMA-BUF buffer -> output P010 planes. */
    VkShaderModule          native_p010_direct_shader;
    VkShaderModule          native_p010_to_nv21_shader;
    VkShaderModule          native_yuv8_to_p010_shader;
    VkShaderModule          native_yuv8_to_nv21_shader;
    VkShaderModule          native_amly_to_nv21_shader;
    VkPipelineLayout        native_p010_direct_pipeline_layout;
    VkPipeline              native_p010_direct_pipeline;
    VkPipeline              native_p010_to_nv21_pipeline;
    VkPipeline              native_yuv8_to_p010_pipeline;
    VkPipeline              native_yuv8_to_nv21_pipeline;
    VkPipeline              native_amly_to_nv21_pipeline;
    VkDescriptorSetLayout   native_p010_direct_ds_layout;
    VkDescriptorPool        native_p010_direct_ds_pool;
    VkDescriptorSet         native_p010_direct_ds[SBS_NATIVE_CANVAS_RING_SIZE][SBS_NATIVE_LAYER_DESCRIPTOR_SETS];
    VkDescriptorSet         native_p010_direct_preview_ds[SBS_NATIVE_CANVAS_RING_SIZE][SBS_NATIVE_LAYER_DESCRIPTOR_SETS];

    /* Native final-output preview downscale: main Y/UV planes -> preview Y/UV planes. */
    VkShaderModule          native_downscale_sdr_shader;
    VkShaderModule          native_downscale_hdr_shader;
    VkPipelineLayout        native_downscale_pipeline_layout;
    VkPipeline              native_downscale_sdr_pipeline;
    VkPipeline              native_downscale_hdr_pipeline;
    VkDescriptorSetLayout   native_downscale_ds_layout;
    VkDescriptorPool        native_downscale_ds_pool;
    VkDescriptorSet         native_downscale_ds[SBS_NATIVE_CANVAS_RING_SIZE];

    /* Upload command buffer — reused each frame for source texture uploads */
    VkCommandBuffer         upload_cb;
    VkFence                 upload_fence;

    /* Fence for staging-image export (render target → host-readable copy).
     * Separate from upload_fence so export and upload can be in-flight
     * without aliasing the same fence. */
    VkFence                 export_fence;

    /* Export compute pipeline for GPU RGBA→NV21 conversion.
     * Replaces the CPU NEON path (28ms → ~2ms). */
    VkShaderModule          export_compute_shader;
    VkPipelineLayout        export_compute_pipeline_layout;
    VkPipeline              export_compute_pipeline;
    VkDescriptorSetLayout   export_compute_ds_layout;
    VkDescriptorPool        export_compute_descriptor_pool;
    VkDescriptorSet         export_compute_ds[SBS_RENDER_TARGET_COUNT];

    /* NV21 export buffers — one per render target, HOST_VISIBLE */
    VkBuffer                nv21_buffer[SBS_RENDER_TARGET_COUNT];
    VkDeviceMemory          nv21_memory[SBS_RENDER_TARGET_COUNT];
    void                   *nv21_mapped[SBS_RENDER_TARGET_COUNT];
    VkDeviceSize            nv21_size;

    VkFence                 compute_fence;
    VkCommandBuffer         compute_cb;
    bool                    nv21_compute_available;

    /* ── P010 compute conversion (post-render-pass) ─────────── */
    /* Compute pipeline that reads the composed RGBA render target
     * and writes P010 (BT.2020 HDR10) to a HOST_VISIBLE SSBO.
     * Dispatched after the render pass ends, before fence submit.
     * No atomics — each work item owns a 2x2 pixel block. */
    VkDescriptorSetLayout   p010_conv_ds_layout;
    VkDescriptorPool        p010_conv_ds_pool;
    VkDescriptorSet         p010_conv_ds[SBS_RENDER_TARGET_COUNT];
    bool                    p010_conv_available;

    /* P010 export buffers — one per render target, HOST_VISIBLE */
    VkBuffer                p010_buffer[SBS_RENDER_TARGET_COUNT];
    VkDeviceMemory          p010_memory[SBS_RENDER_TARGET_COUNT];
    void                   *p010_mapped[SBS_RENDER_TARGET_COUNT];
    VkDeviceSize            p010_size;

    /* Preview NV21 export — GPU compute on blitted preview target */
    VkBuffer                preview_nv21_buffer;
    VkDeviceMemory          preview_nv21_memory;
    void                   *preview_nv21_mapped;
    VkDeviceSize            preview_nv21_size;
    VkImageView             preview_target_view;    /* storage image view for compute */
    VkDescriptorSet         preview_compute_ds;
    bool                    preview_compute_available;

    /* ── New destination-oriented export pipeline ──────────────── */
    /* Sampler-based compute pipeline (export_nv21.comp) that fuses
     * resize + RGBA→NV21 conversion in a single pass. */
    VkShaderModule          dest_export_shader;
    VkPipelineLayout        dest_export_pipeline_layout;
    VkPipeline              dest_export_pipeline;
    VkDescriptorSetLayout   dest_export_ds_layout;
    bool                    dest_export_available;

    /* P010 dest-export pipeline (p010_export.comp) for HDR10 output */
    VkShaderModule          p010_dest_export_shader;
    VkPipelineLayout        p010_dest_export_pipeline_layout;
    VkPipeline              p010_dest_export_pipeline;
    bool                    p010_dest_export_available;

    /* Per-destination export state */
    sbs_export_dest_t       export_dests[SBS_EXPORT_DEST_COUNT];
} sbs_compositor_t;

int  sbs_compositor_init(sbs_compositor_t *comp, uint32_t width, uint32_t height,
                         sbs_export_color_mode_t native_color_mode);
void sbs_compositor_destroy(sbs_compositor_t *comp);
int  sbs_compositor_render_frame(sbs_compositor_t *comp, const sbs_comp_scene_state_t *scene);
int  sbs_compositor_render_native_frame(sbs_compositor_t *comp,
                                         const sbs_comp_scene_state_t *scene,
                                         uint64_t frame_number,
                                         uint64_t content_frame_number,
                                         uint32_t *entry_idx_out);
int  sbs_compositor_load_shaders(sbs_compositor_t *comp, const char *shader_dir);
int  sbs_compositor_configure_native_preview(sbs_compositor_t *comp,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t frame_interval);

/* Native canvas ring helpers.  These deliberately operate on native_canvas
 * entries rather than legacy RGB render targets. */
int  sbs_compositor_native_canvas_acquire(sbs_compositor_t *comp,
                                          uint32_t *entry_idx_out,
                                          sbs_native_canvas_entry_t **entry_out);
void sbs_compositor_native_canvas_mark_rendering(sbs_compositor_t *comp,
                                                 uint32_t entry_idx);
void sbs_compositor_native_canvas_mark_ready(sbs_compositor_t *comp,
                                             uint32_t entry_idx,
                                             uint64_t frame_number);
bool sbs_compositor_native_canvas_republish_last(sbs_compositor_t *comp,
                                                 uint64_t frame_number);
bool sbs_compositor_native_canvas_ref(sbs_compositor_t *comp,
                                       uint32_t entry_idx);
void sbs_compositor_native_canvas_unref(sbs_compositor_t *comp,
                                         uint32_t entry_idx);
bool sbs_compositor_native_canvas_mailbox_acquire(sbs_compositor_t *comp,
                                                  sbs_native_canvas_mailbox_type_t type,
                                                  uint32_t *entry_idx_out,
                                                  uint64_t *frame_number_out,
                                                  sbs_native_canvas_entry_t **entry_out);
void sbs_compositor_native_canvas_mailbox_release(sbs_compositor_t *comp,
                                                  uint32_t entry_idx);

/* Upload YUV frame data from a DMA-BUF fd into a source texture slot.
 * force_sampleable disables native direct YUV fast paths so GPU filters can
 * sample an intermediate RGBA texture. Returns 0 on success. */
int  sbs_compositor_upload_source(sbs_compositor_t *comp, uint32_t slot,
                                    const char *source_id,
                                    int fd, int fd2, uint32_t width, uint32_t height,
                                    uint32_t drm_format, uint64_t drm_modifier,
                                    const uint32_t plane_offset[2],
                                    const uint32_t plane_stride[2],
                                    bool force_sampleable,
                                    uint32_t filter_flags);

/* Drop a frame-scoped source DMA-BUF import after the render fence has
 * completed, before the source buffer is released back to its producer. */
void sbs_compositor_release_source_dmabuf_import(sbs_compositor_t *comp,
                                                 uint32_t slot,
                                                 int cache_idx,
                                                 ino_t inode);

/* Ensure source texture slot is allocated at the given dimensions. */
int  sbs_compositor_alloc_source(sbs_compositor_t *comp, uint32_t slot,
                                  uint32_t width, uint32_t height);

int  sbs_compositor_export_target_fd(sbs_compositor_t *comp, uint32_t target_idx, int *fd);
int  sbs_compositor_export_target_nv21_ptr(sbs_compositor_t *comp,
                                            uint32_t target_idx,
                                            const void **data_out,
                                            size_t *size_out,
                                            uint32_t *target_idx_out);
int  sbs_compositor_export_target_preview_fd(sbs_compositor_t *comp,
                                             uint32_t target_idx,
                                             uint32_t width,
                                             uint32_t height,
                                             int *fd);
int  sbs_compositor_export_target_preview_ptr(sbs_compositor_t *comp,
                                               uint32_t target_idx,
                                               uint32_t width,
                                               uint32_t height,
                                               const void **data_out,
                                               size_t *size_out);

/* ── Destination-oriented export API ──────────────────────────── */

/**
 * Initialize a destination export path.
 * Creates GPU pipeline resources and slot ring for the given destination.
 * Returns -1 on failure (hard error — no fallback).
 */
int  sbs_compositor_init_export_dest(sbs_compositor_t *comp,
                                     sbs_export_dest_type_t type,
                                     uint32_t width, uint32_t height,
                                     sbs_export_color_mode_t color_mode);

/**
 * Submit an export pass for a destination.
 * Records compute dispatch to the next free slot, submits to GPU.
 * Returns the slot index, or -1 if ring is full (frame dropped).
 */
int  sbs_compositor_export_dest_submit(sbs_compositor_t *comp,
                                        sbs_export_dest_type_t type,
                                        uint32_t render_target_idx,
                                        uint64_t frame_number);

/**
 * Acquire the next ready frame from a destination.
 * Returns the slot with completed NV21 data, or NULL if none ready.
 * Caller must call sbs_compositor_export_dest_release() when done.
 */
sbs_export_slot_t *sbs_compositor_export_dest_acquire(sbs_compositor_t *comp,
                                                       sbs_export_dest_type_t type);

/**
 * Release a slot back to the destination ring after consumption.
 */
void sbs_compositor_export_dest_release(sbs_compositor_t *comp,
                                         sbs_export_dest_type_t type,
                                         sbs_export_slot_t *slot);

/**
 * Check if destination export is available.
 */
bool sbs_compositor_dest_export_available(const sbs_compositor_t *comp,
                                           sbs_export_dest_type_t type);

#endif
