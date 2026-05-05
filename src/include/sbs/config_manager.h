#ifndef SBS_CONFIG_MANAGER_H
#define SBS_CONFIG_MANAGER_H

#include <glib.h>
#include <stdint.h>

#include "sbs/scene_graph.h"

typedef struct cJSON cJSON;
typedef struct sbs_api_server sbs_api_server_t;

typedef struct sbs_config_manager sbs_config_manager_t;

sbs_config_manager_t *sbs_config_manager_new(const char *config_dir);
void sbs_config_manager_free(sbs_config_manager_t *mgr);

const char *sbs_config_manager_config_dir(const sbs_config_manager_t *mgr);
int sbs_config_manager_apply_system_overrides(sbs_config_manager_t *mgr,
                                              sbs_scene_graph_t *graph,
                                              uint16_t *api_port);
int sbs_config_manager_preload_canvas(sbs_config_manager_t *mgr,
                                      sbs_scene_graph_t *graph);

cJSON *sbs_config_manager_build_bundle(sbs_api_server_t *server);
int sbs_config_manager_load_bundle(sbs_config_manager_t *mgr, cJSON **out_bundle);
int sbs_config_manager_save_bundle(sbs_config_manager_t *mgr, cJSON *bundle);
void sbs_config_manager_mark_dirty(sbs_config_manager_t *mgr, sbs_api_server_t *server);
void sbs_config_manager_start_runtime(sbs_api_server_t *server);
int sbs_config_manager_apply_bundle(sbs_config_manager_t *mgr, sbs_api_server_t *server, cJSON *bundle);
int sbs_config_manager_reset(sbs_config_manager_t *mgr, sbs_api_server_t *server);

#endif
