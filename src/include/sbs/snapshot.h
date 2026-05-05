#ifndef SBS_SNAPSHOT_H
#define SBS_SNAPSHOT_H

#include "sbs/ipc.h"
#include "cjson/cJSON.h"

#include <stdbool.h>

typedef struct sbs_snapshot_engine sbs_snapshot_engine_t;

sbs_snapshot_engine_t *sbs_snapshot_engine_new(void);
void sbs_snapshot_engine_free(sbs_snapshot_engine_t *engine);
void sbs_snapshot_engine_set_api_port(sbs_snapshot_engine_t *engine, uint16_t port);
bool sbs_snapshot_engine_needs_frame(const sbs_snapshot_engine_t *engine);
void sbs_snapshot_engine_consume_frame(sbs_snapshot_engine_t *engine,
                                       const sbs_video_frame_msg_t *msg,
                                       int memfd);
int sbs_snapshot_engine_capture(sbs_snapshot_engine_t *engine,
                                const char *format,
                                cJSON **metadata);

#endif
