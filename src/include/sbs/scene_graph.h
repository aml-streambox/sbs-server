#ifndef SBS_SCENE_GRAPH_H
#define SBS_SCENE_GRAPH_H

#include "sbs/types.h"
#include "sbs/frame_slot.h"
#include "sbs/compositor_scene.h"

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct cJSON cJSON;

#define SBS_SCENE_GRAPH_MAX_ID 128

typedef enum sbs_scene_color_mode {
    SBS_SCENE_COLOR_MODE_SDR = 0,
    SBS_SCENE_COLOR_MODE_HDR10 = 1,
} sbs_scene_color_mode_t;

typedef enum sbs_source_kind {
    SBS_SOURCE_KIND_STREAMBOXSRC = 0,
    SBS_SOURCE_KIND_V4L2SRC = 1,
    SBS_SOURCE_KIND_URIDECODEBIN = 2,
    SBS_SOURCE_KIND_VIDEOTESTSRC = 3,
    SBS_SOURCE_KIND_IMAGE = 4,
    SBS_SOURCE_KIND_TEXT = 5,
    SBS_SOURCE_KIND_VFMCAP = 6,
    SBS_SOURCE_KIND_ALSA_AUDIO = 7,
} sbs_source_kind_t;

typedef enum sbs_transition_kind {
    SBS_TRANSITION_KIND_CUT = 0,
    SBS_TRANSITION_KIND_FADE = 1,
    SBS_TRANSITION_KIND_SLIDE = 2,
} sbs_transition_kind_t;

typedef struct sbs_canvas_state {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    sbs_scene_color_mode_t color_mode;
    char background_color[16];
} sbs_canvas_state_t;

typedef struct sbs_audio_binding {
    bool enabled;
    char *device;
    double volume;
    double left_gain;
    double right_gain;
    int32_t delay_ms;
    double eq_bands[10];
    bool mute;
    bool monitor;
} sbs_audio_binding_t;

typedef struct sbs_source_state {
    char *id;
    char *name;
    sbs_source_kind_t kind;
    bool enabled;
    bool keep_alive;
    GHashTable *config;
    GPtrArray *filters;
    sbs_audio_binding_t audio;

    sbs_frame_slot_t frame_slot;
    bool slot_initialized;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t color_depth;
    bool hdr;
    char color_space[32];
    char hdr_eotf[32];
    uint32_t drm_format;
    uint64_t drm_modifier;
    uint32_t plane_offset[2];
    uint32_t plane_stride[2];
    bool running;
    bool muted;
    char *runtime_state;
    char *v4l2_effective_decode_mode;
    char *error_message;
} sbs_source_state_t;

typedef struct sbs_scene_item_transform {
    int position_x;
    int position_y;
    int width;
    int height;
    int crop_top;
    int crop_bottom;
    int crop_left;
    int crop_right;
    double rotation_deg;
    bool flip_horizontal;
    bool flip_vertical;
    char *bounds_type;
    char *alignment;
    double opacity;
} sbs_scene_item_transform_t;

typedef struct sbs_filter_state {
    char *id;
    char *type;
    bool enabled;
    GHashTable *params;
} sbs_filter_state_t;

typedef struct sbs_filter_create_params {
    const char *id;
    const char *type;
    bool enabled;
    GHashTable *params;
} sbs_filter_create_params_t;

typedef struct sbs_filter_update_params {
    bool set_enabled;
    bool enabled;
    GHashTable *params;
} sbs_filter_update_params_t;

typedef struct sbs_scene_item_state {
    char *id;
    char *source_id;
    bool visible;
    bool locked;
    int z_order;
    sbs_scene_item_transform_t transform;
    sbs_audio_binding_t audio;
    GPtrArray *filters;
} sbs_scene_item_state_t;

typedef struct sbs_scene_state {
    char *id;
    char *name;
    GPtrArray *items;
    GPtrArray *filters;
} sbs_scene_state_t;

typedef struct sbs_sink_state {
    char *id;
    char *type;
    bool enabled;
    GHashTable *config;
} sbs_sink_state_t;

typedef struct sbs_output_state {
    char *id;
    char *name;
    bool enabled;
    bool autostart;
    GHashTable *encoder;
    GPtrArray *sinks;
    bool running;
    char *runtime_state;
} sbs_output_state_t;

typedef struct sbs_transition_state {
    char *id;
    sbs_transition_kind_t kind;
    uint32_t duration_ms;
    GHashTable *params;
} sbs_transition_state_t;

typedef struct sbs_transition_runtime {
    bool active;
    char *from_scene_id;
    char *to_scene_id;
    char *transition_id;
    sbs_transition_kind_t kind;
    uint32_t duration_ms;
    double progress;
    int64_t start_time_us;
} sbs_transition_runtime_t;

typedef struct sbs_scene_graph {
    sbs_canvas_state_t canvas;
    GHashTable *sources;
    GHashTable *scenes;
    GHashTable *outputs;
    GHashTable *transitions;
    char *active_scene_id;
    char *preview_scene_id;
    char *default_transition_id;
    uint64_t version;
    sbs_transition_runtime_t transition_runtime;
} sbs_scene_graph_t;

typedef struct sbs_source_create_params {
    const char *id;
    const char *name;
    sbs_source_kind_t kind;
    bool enabled;
    bool keep_alive;
    GHashTable *config;
} sbs_source_create_params_t;

typedef struct sbs_source_update_params {
    bool set_name;
    const char *name;
    bool set_enabled;
    bool enabled;
    bool set_keep_alive;
    bool keep_alive;
    bool set_config;
    GHashTable *config;
} sbs_source_update_params_t;

typedef struct sbs_scene_create_params {
    const char *id;
    const char *name;
} sbs_scene_create_params_t;

typedef struct sbs_output_create_params {
    const char *id;
    const char *name;
    bool enabled;
    bool autostart;
} sbs_output_create_params_t;

typedef struct sbs_transition_create_params {
    const char *id;
    sbs_transition_kind_t kind;
    uint32_t duration_ms;
} sbs_transition_create_params_t;

typedef struct sbs_scene_item_create_params {
    const char *id;
    const char *source_id;
    bool visible;
    bool locked;
    int z_order;
    sbs_scene_item_transform_t transform;
    const sbs_audio_binding_t *audio;
} sbs_scene_item_create_params_t;

typedef struct sbs_scene_item_update_params {
    bool set_visible;
    bool visible;
    bool set_locked;
    bool locked;
    bool set_z_order;
    int z_order;
    bool set_transform;
    sbs_scene_item_transform_t transform;
} sbs_scene_item_update_params_t;

sbs_scene_graph_t *sbs_scene_graph_new_default(void);
void sbs_scene_graph_free(sbs_scene_graph_t *graph);

sbs_source_state_t *sbs_scene_graph_get_source(const sbs_scene_graph_t *graph,
                                               const char *id);
sbs_scene_state_t *sbs_scene_graph_get_scene(const sbs_scene_graph_t *graph,
                                             const char *id);
sbs_scene_item_state_t *sbs_scene_graph_get_item(const sbs_scene_graph_t *graph,
                                                 const char *scene_id,
                                                 const char *item_id);
sbs_output_state_t *sbs_scene_graph_get_output(const sbs_scene_graph_t *graph,
                                               const char *id);
sbs_transition_state_t *sbs_scene_graph_get_transition(const sbs_scene_graph_t *graph,
                                                       const char *id);

int sbs_scene_graph_create_source(sbs_scene_graph_t *graph,
                                  const sbs_source_create_params_t *params,
                                  sbs_source_state_t **out_source);
int sbs_scene_graph_update_source(sbs_scene_graph_t *graph,
                                  const char *id,
                                  const sbs_source_update_params_t *params,
                                  sbs_source_state_t **out_source);
int sbs_scene_graph_remove_source(sbs_scene_graph_t *graph, const char *id);
sbs_source_state_t *sbs_scene_graph_steal_source(sbs_scene_graph_t *graph, const char *id);
void source_state_free(gpointer data);

int sbs_scene_graph_create_scene(sbs_scene_graph_t *graph,
                                 const sbs_scene_create_params_t *params,
                                 sbs_scene_state_t **out_scene);
int sbs_scene_graph_update_scene_name(sbs_scene_graph_t *graph,
                                      const char *id,
                                      const char *name);
int sbs_scene_graph_remove_scene(sbs_scene_graph_t *graph, const char *id);

int sbs_scene_graph_add_item(sbs_scene_graph_t *graph,
                             const char *scene_id,
                             const sbs_scene_item_create_params_t *params,
                             sbs_scene_item_state_t **out_item);
int sbs_scene_graph_update_item(sbs_scene_graph_t *graph,
                                const char *scene_id,
                                const char *item_id,
                                const sbs_scene_item_update_params_t *params,
                                sbs_scene_item_state_t **out_item);
int sbs_scene_graph_update_item_audio(sbs_scene_graph_t *graph,
                                      const char *scene_id,
                                      const char *item_id,
                                      const sbs_audio_binding_t *audio,
                                      sbs_scene_item_state_t **out_item);
int sbs_scene_graph_remove_item(sbs_scene_graph_t *graph,
                                const char *scene_id,
                                const char *item_id);
int sbs_scene_graph_reorder_items(sbs_scene_graph_t *graph,
                                  const char *scene_id,
                                  const char * const *item_ids,
                                  size_t item_count);
int sbs_scene_graph_add_filter(sbs_scene_graph_t *graph,
                               const char *source_id,
                               const sbs_filter_create_params_t *params,
                               sbs_filter_state_t **out_filter);
int sbs_scene_graph_update_filter(sbs_scene_graph_t *graph,
                                  const char *source_id,
                                  const char *filter_id,
                                  const sbs_filter_update_params_t *params,
                                  sbs_filter_state_t **out_filter);
int sbs_scene_graph_remove_filter(sbs_scene_graph_t *graph,
                                  const char *source_id,
                                  const char *filter_id);
int sbs_scene_graph_add_scene_filter(sbs_scene_graph_t *graph,
                                     const char *scene_id,
                                     const sbs_filter_create_params_t *params,
                                     sbs_filter_state_t **out_filter);
int sbs_scene_graph_update_scene_filter(sbs_scene_graph_t *graph,
                                        const char *scene_id,
                                        const char *filter_id,
                                        const sbs_filter_update_params_t *params,
                                        sbs_filter_state_t **out_filter);
int sbs_scene_graph_remove_scene_filter(sbs_scene_graph_t *graph,
                                        const char *scene_id,
                                        const char *filter_id);

int sbs_scene_graph_create_output(sbs_scene_graph_t *graph,
                                  const sbs_output_create_params_t *params,
                                  sbs_output_state_t **out_output);
int sbs_scene_graph_remove_output(sbs_scene_graph_t *graph, const char *id);

int sbs_scene_graph_create_transition(sbs_scene_graph_t *graph,
                                      const sbs_transition_create_params_t *params,
                                      sbs_transition_state_t **out_transition);
int sbs_scene_graph_update_transition(sbs_scene_graph_t *graph,
                                      const char *transition_id,
                                      uint32_t duration_ms,
                                      sbs_transition_state_t **out_transition);
int sbs_scene_graph_set_default_transition(sbs_scene_graph_t *graph,
                                           const char *transition_id);
int sbs_scene_graph_set_active_scene(sbs_scene_graph_t *graph,
                                     const char *scene_id,
                                     const char *transition_id);
int sbs_scene_graph_set_preview_scene(sbs_scene_graph_t *graph,
                                      const char *scene_id);
int sbs_scene_graph_transition_to_preview(sbs_scene_graph_t *graph,
                                          const char *transition_id);
void sbs_scene_graph_update_transition_runtime(sbs_scene_graph_t *graph,
                                               int64_t now_us);

cJSON *sbs_scene_graph_serialize_source(const sbs_source_state_t *source);
cJSON *sbs_scene_graph_serialize_scene(const sbs_scene_state_t *scene);
cJSON *sbs_scene_graph_serialize_output(const sbs_output_state_t *output);
cJSON *sbs_scene_graph_serialize_output_public(const sbs_output_state_t *output);
cJSON *sbs_scene_graph_serialize_transition(const sbs_transition_state_t *transition);
cJSON *sbs_scene_graph_serialize_full_state(const sbs_scene_graph_t *graph);
cJSON *sbs_scene_graph_serialize_full_state_public(const sbs_scene_graph_t *graph);
int sbs_scene_graph_build_compositor_state(const sbs_scene_graph_t *graph,
                                           sbs_comp_scene_state_t *state);

const char *sbs_scene_graph_source_kind_name(sbs_source_kind_t kind);
bool sbs_scene_graph_parse_source_kind(const char *type, sbs_source_kind_t *out_kind);
const char *sbs_scene_graph_transition_kind_name(sbs_transition_kind_t kind);
int sbs_scene_graph_count_source_refs(const sbs_scene_graph_t *graph, const char *source_id);

#endif
