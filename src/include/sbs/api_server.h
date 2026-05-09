#ifndef SBS_API_SERVER_H
#define SBS_API_SERVER_H

#include "sbs/scene_graph.h"
#include "sbs/source_supervisor.h"
#include "sbs/output_supervisor.h"
#include "sbs/compositor_thread.h"
#include "sbs/output_router.h"
#include "sbs/preview.h"
#include "sbs/snapshot.h"
#include "sbs/audio_mixer.h"
#include "sbs/config_manager.h"
#include "sbs/instance_manager.h"
#include "sbs/encoder_manager.h"
#include "cjson/cJSON.h"

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct sbs_api_server sbs_api_server_t;
typedef struct sbs_api_client sbs_api_client_t;

typedef int (*sbs_rpc_handler_t)(sbs_api_server_t *server,
                                 sbs_api_client_t *client,
                                 cJSON *params,
                                 cJSON **result,
                                 cJSON **error);

struct sbs_api_client {
    uint64_t id;
    GPtrArray *subscriptions;
    GPtrArray *outbox;
    gpointer connection;
    gpointer thread;
    char *peer_ip;
    bool connected;
};

struct sbs_api_server {
    sbs_scene_graph_t *scene_graph;
    sbs_source_supervisor_t *source_sup;
    sbs_output_supervisor_t *output_sup;
    sbs_compositor_thread_t *comp_thread;
    sbs_output_router_t *output_router;
    sbs_preview_engine_t *preview;
    sbs_snapshot_engine_t *snapshot;
    sbs_audio_mixer_t *audio;
    sbs_config_manager_t *config;
    sbs_instance_manager_t *instance_mgr;
    sbs_encoder_manager_t *encoder_mgr;
    GHashTable *methods;
    GHashTable *clients;
    guint service_timer_id;
    guint telemetry_timer_id;
    guint audio_level_timer_id;
    uint64_t next_client_id;
    int64_t started_monotonic_usec;
    uint64_t last_telemetry_frame_count;
    uint64_t last_telemetry_content_frame_count;
    uint64_t last_telemetry_encoded_bytes;
    int64_t last_telemetry_monotonic_usec;
    double last_telemetry_compositor_fps;
    double last_telemetry_content_fps;
    double last_telemetry_bitrate_kbps;
    double last_telemetry_latency_ms;
    bool last_telemetry_pipeline_slow;
    uint32_t telemetry_slow_streak;
    bool pending_canvas_valid;
    uint32_t pending_canvas_width;
    uint32_t pending_canvas_height;
    uint32_t pending_canvas_fps_num;
    uint32_t pending_canvas_fps_den;
    char *pending_canvas_color_mode;
    char *pending_canvas_background_color;
    bool running;
    gpointer socket_service;
    gpointer preview_socket_service;
    gpointer unix_socket_service;
    uint16_t port;
    uint16_t preview_port;
    uint32_t instance_id;
    char *unix_socket_path;
};

sbs_api_server_t *sbs_api_server_new(sbs_scene_graph_t *scene_graph,
                                     sbs_source_supervisor_t *source_sup,
                                     sbs_output_supervisor_t *output_sup,
                                     sbs_compositor_thread_t *comp_thread,
                                     sbs_output_router_t *output_router);
void sbs_api_server_free(sbs_api_server_t *server);
int sbs_api_server_start(sbs_api_server_t *server, uint16_t port);
int sbs_api_server_start_unix(sbs_api_server_t *server, const char *socket_path);
void sbs_api_server_stop(sbs_api_server_t *server);

sbs_api_client_t *sbs_api_client_new(uint64_t id);
void sbs_api_client_free(sbs_api_client_t *client);
uint64_t sbs_api_server_add_client(sbs_api_server_t *server, sbs_api_client_t *client);
void sbs_api_server_remove_client(sbs_api_server_t *server, uint64_t client_id);

int sbs_api_server_dispatch_json(sbs_api_server_t *server,
                                 sbs_api_client_t *client,
                                 const char *request_json,
                                 char **response_json);

void sbs_api_server_publish(sbs_api_server_t *server,
                            const char *topic,
                            cJSON *data);

bool sbs_api_client_matches_topic(const sbs_api_client_t *client,
                                  const char *topic);
char *sbs_api_client_take_message(sbs_api_client_t *client);

void sbs_api_register_core_methods(sbs_api_server_t *server);
void sbs_api_register_controller_methods(sbs_api_server_t *server);
void sbs_api_server_configure_controller(sbs_api_server_t *server,
                                         sbs_instance_manager_t *instance_mgr);
void sbs_api_server_set_instance_id(sbs_api_server_t *server, uint32_t instance_id);
void sbs_api_server_refresh_scene(sbs_api_server_t *server);
void sbs_api_server_publish_telemetry(sbs_api_server_t *server);

int sbs_api_handle_system_get_state(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_system_get_info(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_list(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_get(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_list_kinds(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_describe_kind(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_discover_v4l2(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_upload_asset(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_source_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_list(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_get(sbs_api_server_t *server, sbs_api_client_t *client,
                             cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_set_active(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_set_preview(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_transition_to_preview(sbs_api_server_t *server, sbs_api_client_t *client,
                                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_transition_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                           cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_item_add(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_item_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_item_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_scene_item_reorder(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_filter_add(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_filter_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_filter_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_list(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_get(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_output_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_canvas_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_canvas_apply(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_pubsub_subscribe(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_pubsub_unsubscribe(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_list_profiles(sbs_api_server_t *server, sbs_api_client_t *client,
                                         cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_ensure_profile(sbs_api_server_t *server, sbs_api_client_t *client,
                                          cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_get_status(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_release_profile(sbs_api_server_t *server, sbs_api_client_t *client,
                                            cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_webrtc_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_webrtc_answer(sbs_api_server_t *server, sbs_api_client_t *client,
                                         cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_webrtc_ice(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_command_execute(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_snapshot_capture(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_audio_get_levels(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_audio_set_source(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_audio_set_scene_item(sbs_api_server_t *server, sbs_api_client_t *client,
                                        cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_audio_set_master(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_config_export(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_config_import(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_config_reset(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_list(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_start(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_stop(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_restart(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_enable(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_disable(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                   cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_get_info(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_get_state(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_instance_call(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_encoder_get_config(sbs_api_server_t *server, sbs_api_client_t *client,
                                       cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_encoder_update_config(sbs_api_server_t *server, sbs_api_client_t *client,
                                           cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_get_encoder_config(sbs_api_server_t *server, sbs_api_client_t *client,
                                               cJSON *params, cJSON **result, cJSON **error);
int sbs_api_handle_preview_update_encoder_config(sbs_api_server_t *server, sbs_api_client_t *client,
                                                  cJSON *params, cJSON **result, cJSON **error);

#endif
