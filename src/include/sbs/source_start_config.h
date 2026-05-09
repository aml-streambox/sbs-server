#ifndef SBS_SOURCE_START_CONFIG_H
#define SBS_SOURCE_START_CONFIG_H

#include "sbs/scene_graph.h"
#include "sbs/source_supervisor.h"

void sbs_source_start_config_fill(const sbs_scene_graph_t *graph,
                                  const sbs_canvas_state_t *canvas,
                                  sbs_source_state_t *source,
                                  sbs_source_start_config_t *cfg);

#endif
