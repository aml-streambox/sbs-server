#include "sbs/spsc_queue.h"
#include "sbs_test.h"
#include <pthread.h>
#include <stdlib.h>

#define NUM_ITEMS   10000
#define QUEUE_CAP   1024

SBS_TEST(spsc, new_returns_valid)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(16);
    SBS_ASSERT_NOT_NULL(q);
    SBS_ASSERT_EQ(sbs_spsc_queue_capacity(q), 15);
    SBS_ASSERT_EQ(sbs_spsc_queue_count(q), 0);
    sbs_spsc_queue_free(q);
}

SBS_TEST(spsc, new_rounds_up_to_pow2)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(10);
    SBS_ASSERT_NOT_NULL(q);
    SBS_ASSERT_EQ(sbs_spsc_queue_capacity(q), 15);
    sbs_spsc_queue_free(q);
}

SBS_TEST(spsc, new_rejects_zero)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(0);
    SBS_ASSERT_NULL(q);
}

SBS_TEST(spsc, push_pop_single)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(4);
    int val = 42;
    SBS_ASSERT(sbs_spsc_queue_push(q, &val));
    SBS_ASSERT_EQ(sbs_spsc_queue_count(q), 1);

    void *out = sbs_spsc_queue_pop(q);
    SBS_ASSERT_NOT_NULL(out);
    SBS_ASSERT_EQ(*((int *)out), 42);
    SBS_ASSERT_EQ(sbs_spsc_queue_count(q), 0);
    sbs_spsc_queue_free(q);
}

SBS_TEST(spsc, pop_empty_returns_null)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(4);
    SBS_ASSERT_NULL(sbs_spsc_queue_pop(q));
    sbs_spsc_queue_free(q);
}

SBS_TEST(spsc, push_full_returns_false)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(4);
    int vals[4];
    for (int i = 0; i < 3; i++) {
        vals[i] = i;
        SBS_ASSERT(sbs_spsc_queue_push(q, &vals[i]));
    }
    SBS_ASSERT(!sbs_spsc_queue_push(q, &vals[3]));
    sbs_spsc_queue_free(q);
}

static void *producer_thread(void *arg)
{
    sbs_spsc_queue_t *q = (sbs_spsc_queue_t *)arg;
    for (int i = 1; i <= NUM_ITEMS; i++) {
        int *val = malloc(sizeof(int));
        *val = i;
        while (!sbs_spsc_queue_push(q, val))
            ;
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    sbs_spsc_queue_t *q = (sbs_spsc_queue_t *)arg;
    int last = 0;
    for (int i = 0; i < NUM_ITEMS; i++) {
        int *val;
        while ((val = (int *)sbs_spsc_queue_pop(q)) == NULL)
            ;
        if (*val != last + 1)
            return NULL;
        last = *val;
        free(val);
    }
    return NULL;
}

SBS_TEST(spsc, concurrent_prod_cons)
{
    sbs_spsc_queue_t *q = sbs_spsc_queue_new(QUEUE_CAP);
    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_thread, q);
    pthread_create(&cons, NULL, consumer_thread, q);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    SBS_ASSERT_EQ(sbs_spsc_queue_count(q), 0);
    sbs_spsc_queue_free(q);
}

SBS_TEST_MAIN()
