#ifndef SBS_COMPOSITOR_SCENE_H
#define SBS_COMPOSITOR_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "sbs/frame_slot.h"

#define SBS_COMP_SCENE_MAX_ITEMS 32
#define SBS_COMP_LUT_PATH_MAX 256

#define SBS_COMP_FILTER_GRAYSCALE       (1u << 0)
#define SBS_COMP_FILTER_BRIGHTNESS      (1u << 1)
#define SBS_COMP_FILTER_CONTRAST        (1u << 2)
#define SBS_COMP_FILTER_BLUR            (1u << 3)
#define SBS_COMP_FILTER_SHARPEN         (1u << 4)
#define SBS_COMP_FILTER_HDR_TO_SDR_LUT  (1u << 5)
#define SBS_COMP_FILTER_COLOR_CORRECTION (1u << 6)
#define SBS_COMP_FILTER_LUMA_KEY        (1u << 7)
#define SBS_COMP_FILTER_CHROMA_KEY      (1u << 8)
#define SBS_COMP_FILTER_LUT             (1u << 9)
#define SBS_COMP_FILTER_SDR_TO_HDR      (1u << 10)

typedef struct sbs_comp_scene_item {
    char source_id[128];
    bool visible;
    int z_order;
    float transform[16];
    float crop[4];
    float opacity;
    float tint[4];
    uint32_t filter_flags;
    float filter_params[8];
    float hdr_to_sdr_saturation;
    float hdr_to_sdr_brightness;
    float hdr_to_sdr_hue_deg;
    char lut_path[SBS_COMP_LUT_PATH_MAX];
    int render_x;
    int render_y;
    int render_width;
    int render_height;
    bool flip_horizontal;
    bool flip_vertical;
    float rotation_deg;

    /* Pointer to the source's frame slot (owned by scene_graph).
     * The compositor thread reads this each tick to check for new frames.
     * NULL means no live source data — render tint-only placeholder. */
    sbs_frame_slot_t *frame_slot;

    /* Source video frame dimensions (needed for NV21→RGBA upload).
     * Populated from source_state->frame_width/frame_height. */
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t color_depth;
    bool hdr;

    /* Source pixel format and plane layout for DMA-BUF import.
     * Populated from source_state->drm_format and plane info. */
    uint32_t drm_format;
    uint64_t drm_modifier;
    uint32_t plane_offset[2];
    uint32_t plane_stride[2];
} sbs_comp_scene_item_t;

typedef struct sbs_comp_scene_state {
    float background_rgba[4];
    uint32_t active_item_count;
    sbs_comp_scene_item_t active_items[SBS_COMP_SCENE_MAX_ITEMS];
    bool transition_active;
    float transition_progress;
    int64_t transition_start_time_us;
    uint32_t transition_duration_ms;
    uint32_t previous_item_count;
    sbs_comp_scene_item_t previous_items[SBS_COMP_SCENE_MAX_ITEMS];
} sbs_comp_scene_state_t;

void sbs_comp_scene_state_init(sbs_comp_scene_state_t *state);

#endif
