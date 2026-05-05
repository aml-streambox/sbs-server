#include "sbs/frame_slot.h"
#include "sbs/ipc.h"
#include "sbs/ipc_transport.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void release_frame(sbs_frame_slot_t *slot, void *frame)
{
    if (!frame)
        return;
    if (slot->release_fn)
        slot->release_fn(frame, slot->release_user_data);
}

static void clear_entry(sbs_frame_slot_t *slot, uint32_t idx, bool count_drop)
{
    void *frame = slot->entries[idx].frame;
    slot->entries[idx].frame = NULL;
    slot->entries[idx].timestamp_us = 0;
    release_frame(slot, frame);
    if (count_drop)
        atomic_fetch_add_explicit(&slot->dropped, 1, memory_order_relaxed);
}

void sbs_frame_slot_init(sbs_frame_slot_t *slot)
{
    memset(slot, 0, sizeof(*slot));
    pthread_mutex_init(&slot->lock, NULL);
}

void sbs_frame_slot_destroy(sbs_frame_slot_t *slot)
{
    if (!slot)
        return;
    sbs_frame_slot_flush(slot);
    pthread_mutex_destroy(&slot->lock);
}

void sbs_frame_slot_set_release_func(sbs_frame_slot_t *slot,
                                     sbs_frame_slot_release_fn release_fn,
                                     void *user_data)
{
    if (!slot)
        return;
    pthread_mutex_lock(&slot->lock);
    slot->release_fn = release_fn;
    slot->release_user_data = user_data;
    pthread_mutex_unlock(&slot->lock);
}

void sbs_frame_slot_publish(sbs_frame_slot_t *slot, void *frame, uint64_t timestamp_us)
{
    void *dropped = sbs_frame_slot_exchange(slot, frame, timestamp_us);
    release_frame(slot, dropped);
}

void *sbs_frame_slot_exchange(sbs_frame_slot_t *slot, void *frame, uint64_t timestamp_us)
{
    void *dropped = NULL;

    if (!slot || !frame)
        return NULL;

    pthread_mutex_lock(&slot->lock);
    atomic_fetch_add_explicit(&slot->produced, 1, memory_order_relaxed);

    if (slot->count == SBS_FRAME_SLOT_RING_SIZE) {
        dropped = slot->entries[slot->head].frame;
        slot->entries[slot->head].frame = NULL;
        slot->entries[slot->head].timestamp_us = 0;
        slot->head = (slot->head + 1u) % SBS_FRAME_SLOT_RING_SIZE;
        slot->count--;
        atomic_fetch_add_explicit(&slot->dropped, 1, memory_order_relaxed);
        if (slot->release_fn) {
            slot->release_fn(dropped, slot->release_user_data);
            dropped = NULL;
        }
    }

    uint32_t tail = (slot->head + slot->count) % SBS_FRAME_SLOT_RING_SIZE;
    slot->entries[tail].frame = frame;
    slot->entries[tail].timestamp_us = timestamp_us;
    slot->count++;
    pthread_mutex_unlock(&slot->lock);
    return dropped;
}

void *sbs_frame_slot_acquire(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us)
{
    void *frame = NULL;

    if (!slot)
        return NULL;

    pthread_mutex_lock(&slot->lock);
    if (slot->count > 0) {
        uint32_t newest = (slot->head + slot->count - 1u) % SBS_FRAME_SLOT_RING_SIZE;
        frame = slot->entries[newest].frame;
        if (out_timestamp_us)
            *out_timestamp_us = slot->entries[newest].timestamp_us;
    } else if (out_timestamp_us) {
        *out_timestamp_us = 0;
    }
    pthread_mutex_unlock(&slot->lock);
    return frame;
}

void *sbs_frame_slot_take(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us)
{
    void *frame = NULL;

    if (!slot)
        return NULL;

    pthread_mutex_lock(&slot->lock);
    if (slot->count == 0) {
        if (out_timestamp_us)
            *out_timestamp_us = 0;
        pthread_mutex_unlock(&slot->lock);
        return NULL;
    }

    uint32_t newest = (slot->head + slot->count - 1u) % SBS_FRAME_SLOT_RING_SIZE;
    for (uint32_t n = 0; n < slot->count; n++) {
        uint32_t idx = (slot->head + n) % SBS_FRAME_SLOT_RING_SIZE;
        if (idx == newest)
            continue;
        clear_entry(slot, idx, true);
    }

    frame = slot->entries[newest].frame;
    if (out_timestamp_us)
        *out_timestamp_us = slot->entries[newest].timestamp_us;
    slot->entries[newest].frame = NULL;
    slot->entries[newest].timestamp_us = 0;
    slot->head = 0;
    slot->count = 0;
    atomic_fetch_add_explicit(&slot->consumed, 1, memory_order_relaxed);
    pthread_mutex_unlock(&slot->lock);
    return frame;
}

void *sbs_frame_slot_take_oldest(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us)
{
    void *frame = NULL;

    if (!slot)
        return NULL;

    pthread_mutex_lock(&slot->lock);
    if (slot->count == 0) {
        if (out_timestamp_us)
            *out_timestamp_us = 0;
        pthread_mutex_unlock(&slot->lock);
        return NULL;
    }

    uint32_t idx = slot->head;
    frame = slot->entries[idx].frame;
    if (out_timestamp_us)
        *out_timestamp_us = slot->entries[idx].timestamp_us;
    slot->entries[idx].frame = NULL;
    slot->entries[idx].timestamp_us = 0;
    slot->head = (slot->head + 1u) % SBS_FRAME_SLOT_RING_SIZE;
    slot->count--;
    if (slot->count == 0)
        slot->head = 0;
    atomic_fetch_add_explicit(&slot->consumed, 1, memory_order_relaxed);
    pthread_mutex_unlock(&slot->lock);
    return frame;
}

void sbs_frame_slot_flush(sbs_frame_slot_t *slot)
{
    if (!slot)
        return;

    pthread_mutex_lock(&slot->lock);
    for (uint32_t n = 0; n < slot->count; n++) {
        uint32_t idx = (slot->head + n) % SBS_FRAME_SLOT_RING_SIZE;
        clear_entry(slot, idx, true);
    }
    slot->head = 0;
    slot->count = 0;
    pthread_mutex_unlock(&slot->lock);
}

void sbs_frame_slot_get_stats(sbs_frame_slot_t *slot, sbs_frame_slot_stats_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!slot)
        return;

    pthread_mutex_lock(&slot->lock);
    out->produced = atomic_load_explicit(&slot->produced, memory_order_relaxed);
    out->dropped = atomic_load_explicit(&slot->dropped, memory_order_relaxed);
    out->consumed = atomic_load_explicit(&slot->consumed, memory_order_relaxed);
    out->held = slot->count;
    pthread_mutex_unlock(&slot->lock);
}

void sbs_frame_fds_release(void *frame, void *user_data)
{
    sbs_frame_fds_t *fds = frame;
    (void)user_data;

    if (!fds)
        return;
    if (fds->needs_release_ack && fds->release_sock_fd >= 0) {
        sbs_frame_release_msg_t msg;
        sbs_frame_release_msg_init(&msg);
        msg.sequence = fds->release_sequence;
        (void)sbs_ipc_send_msg(fds->release_sock_fd, &msg, sizeof(msg));
        close(fds->release_sock_fd);
        fds->release_sock_fd = -1;
    }
    if (fds->dmabuf_fd >= 0)
        close(fds->dmabuf_fd);
    if (fds->dmabuf_fd2 >= 0 && fds->dmabuf_fd2 != fds->dmabuf_fd)
        close(fds->dmabuf_fd2);
    free(fds);
}
