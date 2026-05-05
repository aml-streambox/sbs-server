#include "sbs/spsc_queue.h"

#include <stdlib.h>
#include <string.h>

static uint32_t next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

sbs_spsc_queue_t *sbs_spsc_queue_new(uint32_t capacity)
{
    if (capacity == 0 || capacity > SBS_SPSC_MAX_CAPACITY)
        return NULL;

    sbs_spsc_queue_t *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    q->capacity = next_pow2(capacity);
    q->mask = q->capacity - 1;

    q->slots = calloc(q->capacity, sizeof(void *));
    if (!q->slots) {
        free(q);
        return NULL;
    }

    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);

    return q;
}

void sbs_spsc_queue_free(sbs_spsc_queue_t *q)
{
    if (!q)
        return;
    free(q->slots);
    free(q);
}

bool sbs_spsc_queue_push(sbs_spsc_queue_t *q, void *item)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) & q->mask;
    if (next == atomic_load_explicit(&q->head, memory_order_acquire))
        return false;

    q->slots[tail] = item;
    atomic_store_explicit(&q->tail, next, memory_order_release);

    return true;
}

void *sbs_spsc_queue_pop(sbs_spsc_queue_t *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (head == atomic_load_explicit(&q->tail, memory_order_acquire))
        return NULL;

    void *item = q->slots[head];
    q->slots[head] = NULL;
    atomic_store_explicit(&q->head, (head + 1) & q->mask, memory_order_release);

    return item;
}

uint32_t sbs_spsc_queue_count(const sbs_spsc_queue_t *q)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    return (tail - head) & q->mask;
}

uint32_t sbs_spsc_queue_capacity(const sbs_spsc_queue_t *q)
{
    return q->capacity - 1;
}
