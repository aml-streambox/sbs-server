#ifndef SBS_SPSC_QUEUE_H
#define SBS_SPSC_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define SBS_SPSC_MAX_CAPACITY 4096

typedef struct sbs_spsc_queue {
    void               **slots;
    uint32_t             capacity;
    uint32_t             mask;
    _Atomic uint32_t     head;
    _Atomic uint32_t     tail;
} sbs_spsc_queue_t;

sbs_spsc_queue_t *sbs_spsc_queue_new(uint32_t capacity);
void              sbs_spsc_queue_free(sbs_spsc_queue_t *q);
bool              sbs_spsc_queue_push(sbs_spsc_queue_t *q, void *item);
void             *sbs_spsc_queue_pop(sbs_spsc_queue_t *q);
uint32_t          sbs_spsc_queue_count(const sbs_spsc_queue_t *q);
uint32_t          sbs_spsc_queue_capacity(const sbs_spsc_queue_t *q);

#endif
