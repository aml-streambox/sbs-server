#ifndef SBS_INSTANCE_MANAGER_H
#define SBS_INSTANCE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "cjson/cJSON.h"

typedef struct sbs_instance_manager sbs_instance_manager_t;

typedef struct sbs_instance_info {
    uint32_t instance_id;
    const char *name;
    bool desired_running;
    bool running;
    pid_t pid;
    uint16_t api_port;
    uint16_t preview_port;
    const char *config_dir;
    const char *control_socket_path;
    const char *log_path;
} sbs_instance_info_t;

sbs_instance_manager_t *sbs_instance_manager_new(const char *server_bin,
                                                 const char *controller_config_dir,
                                                 const char *socket_root,
                                                 const char *log_level);
void sbs_instance_manager_free(sbs_instance_manager_t *mgr);

int sbs_instance_manager_load(sbs_instance_manager_t *mgr);
int sbs_instance_manager_save(sbs_instance_manager_t *mgr);
int sbs_instance_manager_ensure_default(sbs_instance_manager_t *mgr);

size_t sbs_instance_manager_count(sbs_instance_manager_t *mgr);
int sbs_instance_manager_get_info(sbs_instance_manager_t *mgr,
                                  uint32_t instance_id,
                                  sbs_instance_info_t *info);
cJSON *sbs_instance_manager_build_inventory(sbs_instance_manager_t *mgr);

int sbs_instance_manager_create_instance(sbs_instance_manager_t *mgr,
                                         const char *name,
                                         uint32_t *out_instance_id);
int sbs_instance_manager_update_instance(sbs_instance_manager_t *mgr,
                                         uint32_t instance_id,
                                         const char *name,
                                         bool *desired_running);
int sbs_instance_manager_start_instance(sbs_instance_manager_t *mgr,
                                        uint32_t instance_id);
int sbs_instance_manager_stop_instance(sbs_instance_manager_t *mgr,
                                       uint32_t instance_id);
int sbs_instance_manager_remove_instance(sbs_instance_manager_t *mgr,
                                         uint32_t instance_id);
int sbs_instance_manager_start_desired(sbs_instance_manager_t *mgr);
void sbs_instance_manager_shutdown_all(sbs_instance_manager_t *mgr);

int sbs_instance_manager_call_json(sbs_instance_manager_t *mgr,
                                   uint32_t instance_id,
                                   const char *request_json,
                                   char **response_json);

#endif
