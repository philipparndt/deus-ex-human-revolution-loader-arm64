// Running host work on the main thread for guest threads.
//
// dispatch_sync to the main queue deadlocks when the guest's main thread
// busy-waits for the calling thread without returning to the run loop (Feral
// spins in a ShowWindow loop while the game thread creates its NSWindow). Work
// is queued here instead and drained both from the main run loop and on every
// thunk entry on the main thread (host_dispatch), i.e. at guest API calls.
#include "mainq.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct mq_item {
    struct mq_item *next;
    void (^blk)(void);
    dispatch_semaphore_t done; // NULL: asynchronous, the item is heap-allocated
    void (*fn)(void *);        // asynchronous function form (instead of blk)
    void *ctx;
};

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mq_item *mq_head, **mq_tail = &mq_head;
atomic_int mainq_pending;

void mainq_drain(void) {
    for (;;) {
        pthread_mutex_lock(&mq_lock);
        struct mq_item *it = mq_head;
        if (it) {
            mq_head = it->next;
            if (!mq_head) mq_tail = &mq_head;
            atomic_fetch_sub(&mainq_pending, 1);
        }
        pthread_mutex_unlock(&mq_lock);
        if (!it) return;
        if (it->fn) it->fn(it->ctx);
        else it->blk();
        if (it->done) dispatch_semaphore_signal(it->done);
        else free(it);
    }
}

static void mq_push(struct mq_item *it) {
    pthread_mutex_lock(&mq_lock);
    *mq_tail = it;
    mq_tail = &it->next;
    atomic_fetch_add(&mainq_pending, 1);
    pthread_mutex_unlock(&mq_lock);
    dispatch_async(dispatch_get_main_queue(), ^{
      mainq_drain(); // run loop path (no-op if a thunk entry got there first)
    });
}

void mainq_async_f(void *ctx, void (*fn)(void *)) {
    struct mq_item *it = calloc(1, sizeof *it);
    it->fn = fn;
    it->ctx = ctx;
    mq_push(it);
}

void mainq_run(void (^blk)(void)) {
    if (pthread_main_np()) {
        blk();
        return;
    }
    struct mq_item it = {.blk = blk, .done = dispatch_semaphore_create(0)};
    mq_push(&it);
    dispatch_semaphore_wait(it.done, DISPATCH_TIME_FOREVER);
    dispatch_release(it.done);
}
