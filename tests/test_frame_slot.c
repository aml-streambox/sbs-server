#include "sbs/frame_slot.h"
#include "sbs_test.h"
#include <pthread.h>
#include <stdint.h>

SBS_TEST(frame_slot, init_null)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);
    void *f = sbs_frame_slot_acquire(&slot, NULL);
    SBS_ASSERT_NULL(f);
}

SBS_TEST(frame_slot, publish_acquire)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    int val = 42;
    sbs_frame_slot_publish(&slot, &val, 1000);

    uint64_t ts = 0;
    void *f = sbs_frame_slot_acquire(&slot, &ts);
    SBS_ASSERT_NOT_NULL(f);
    SBS_ASSERT_EQ(*((int *)f), 42);
    SBS_ASSERT_EQ(ts, 1000);
}

SBS_TEST(frame_slot, overwrite)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    int a = 1, b = 2;
    sbs_frame_slot_publish(&slot, &a, 100);
    sbs_frame_slot_publish(&slot, &b, 200);

    void *f = sbs_frame_slot_acquire(&slot, NULL);
    SBS_ASSERT_EQ(*((int *)f), 2);
}

SBS_TEST(frame_slot, exchange_returns_dropped_when_full)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    int a = 1, b = 2, c = 3, d = 4, e = 5;
    sbs_frame_slot_publish(&slot, &a, 100);
    sbs_frame_slot_publish(&slot, &b, 200);
    sbs_frame_slot_publish(&slot, &c, 300);
    sbs_frame_slot_publish(&slot, &d, 400);

    void *old = sbs_frame_slot_exchange(&slot, &e, 500);
    uint64_t ts = 0;
    void *cur = sbs_frame_slot_acquire(&slot, &ts);

    SBS_ASSERT_EQ(*((int *)old), 1);
    SBS_ASSERT_EQ(*((int *)cur), 5);
    SBS_ASSERT_EQ(ts, 500);
}

SBS_TEST(frame_slot, take_clears_slot)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    int a = 7;
    uint64_t ts = 0;
    sbs_frame_slot_publish(&slot, &a, 321);

    void *taken = sbs_frame_slot_take(&slot, &ts);
    void *cur = sbs_frame_slot_acquire(&slot, NULL);

    SBS_ASSERT_EQ(*((int *)taken), 7);
    SBS_ASSERT_EQ(ts, 321);
    SBS_ASSERT_NULL(cur);
}

static void *publisher_thread(void *arg)
{
    sbs_frame_slot_t *slot = (sbs_frame_slot_t *)arg;
    for (int i = 1; i <= 10000; i++)
        sbs_frame_slot_publish(slot, (void *)(uintptr_t)i, (uint64_t)i);
    return NULL;
}

SBS_TEST(frame_slot, concurrent_publish)
{
    sbs_frame_slot_t slot;
    sbs_frame_slot_init(&slot);

    pthread_t t;
    pthread_create(&t, NULL, publisher_thread, &slot);
    pthread_join(t, NULL);

    uint64_t ts = 0;
    void *f = sbs_frame_slot_acquire(&slot, &ts);
    SBS_ASSERT_EQ((uintptr_t)f, 10000);
    SBS_ASSERT_EQ(ts, 10000);
}

SBS_TEST_MAIN()
