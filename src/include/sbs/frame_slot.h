#ifndef SBS_FRAME_SLOT_H
#define SBS_FRAME_SLOT_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>

#define SBS_FRAME_SLOT_RING_SIZE 4

typedef void (*sbs_frame_slot_release_fn)(void *frame, void *user_data);

typedef struct sbs_frame_slot_stats {
    uint64_t produced;
    uint64_t dropped;
    uint64_t consumed;
    uint32_t held;
} sbs_frame_slot_stats_t;

typedef struct sbs_frame_slot {
    pthread_mutex_t lock;
    struct {
        void    *frame;
        uint64_t timestamp_us;
    } entries[SBS_FRAME_SLOT_RING_SIZE];
    uint32_t head;
    uint32_t count;
    sbs_frame_slot_release_fn release_fn;
    void *release_user_data;
    _Atomic uint64_t produced;
    _Atomic uint64_t dropped;
    _Atomic uint64_t consumed;
} sbs_frame_slot_t;

/* Helper struct for passing 1-2 DMA-BUF fds through the frame slot.
 * When dmabuf_fd2 < 0, only dmabuf_fd is valid. */
typedef struct sbs_frame_fds {
    int  dmabuf_fd;
    int  dmabuf_fd2;
    int  release_sock_fd;
    uint64_t release_sequence;
    bool needs_release_ack;
} sbs_frame_fds_t;

void     sbs_frame_slot_init(sbs_frame_slot_t *slot);
void     sbs_frame_slot_destroy(sbs_frame_slot_t *slot);
void     sbs_frame_slot_set_release_func(sbs_frame_slot_t *slot,
                                         sbs_frame_slot_release_fn release_fn,
                                         void *user_data);
void     sbs_frame_slot_publish(sbs_frame_slot_t *slot, void *frame, uint64_t timestamp_us);
void    *sbs_frame_slot_acquire(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us);
void    *sbs_frame_slot_exchange(sbs_frame_slot_t *slot, void *frame, uint64_t timestamp_us);
void    *sbs_frame_slot_take(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us);
void    *sbs_frame_slot_take_oldest(sbs_frame_slot_t *slot, uint64_t *out_timestamp_us);
void     sbs_frame_slot_flush(sbs_frame_slot_t *slot);
void     sbs_frame_slot_get_stats(sbs_frame_slot_t *slot, sbs_frame_slot_stats_t *out);

void     sbs_frame_fds_release(void *frame, void *user_data);

#endif
