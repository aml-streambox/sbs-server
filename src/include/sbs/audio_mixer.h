#ifndef SBS_AUDIO_MIXER_H
#define SBS_AUDIO_MIXER_H

#include "sbs/scene_graph.h"
#include "sbs/ipc.h"

#include <glib.h>

typedef struct sbs_audio_level_info {
    double level_db;
    double peak_db;
    int64_t timestamp_us;
} sbs_audio_level_info_t;

typedef struct sbs_audio_buffer {
    sbs_audio_buffer_msg_t msg;
    uint8_t data[];
} sbs_audio_buffer_t;

typedef struct sbs_audio_mixer sbs_audio_mixer_t;

sbs_audio_mixer_t *sbs_audio_mixer_new(sbs_scene_graph_t *graph);
void sbs_audio_mixer_free(sbs_audio_mixer_t *audio);
int sbs_audio_mixer_start(sbs_audio_mixer_t *audio);
void sbs_audio_mixer_stop(sbs_audio_mixer_t *audio);

void sbs_audio_mixer_on_scene_change(sbs_audio_mixer_t *audio,
                                     const char *active_scene_id);
int sbs_audio_mixer_set_source_state(sbs_audio_mixer_t *audio,
                                     sbs_source_state_t *source);
int sbs_audio_mixer_set_scene_item_state(sbs_audio_mixer_t *audio,
                                         sbs_scene_item_state_t *item);
int sbs_audio_mixer_set_master(sbs_audio_mixer_t *audio,
                               double volume,
                               bool mute);

cJSON *sbs_audio_mixer_serialize_levels(sbs_audio_mixer_t *audio);
cJSON *sbs_audio_mixer_serialize_state(sbs_audio_mixer_t *audio);
sbs_audio_buffer_t *sbs_audio_mixer_take_latest_buffer(sbs_audio_mixer_t *audio);

#endif
