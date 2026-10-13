// Running host work on the main thread for guest threads.
//
// dispatch_sync to the main queue deadlocks when the guest's main thread
// busy-waits for the calling thread without returning to the run loop (Feral
// spins in a ShowWindow loop while the game thread creates its NSWindow). Work
// is queued here instead and drained from the main run loop, and on every thunk
// entry on the main thread (host_dispatch, i.e. at guest API calls) as far as a
// waiting thread needs it (mainq_drain_waiting). Guest dispatch_async blocks
// otherwise wait for the run loop, as with GCD: run inside whatever guest code
// happens to call the host (e.g. in the middle of an event handler), one that
// waits for another thread deadlocked HR (found by the autoplay bot).
#include "mainq.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct mq_item {
    struct mq_item *next;
    void (^blk)(void);
    dispatch_semaphore_t done; // NULL: asynchronous, the item is heap-allocated
    void (*fn)(void *);        // asynchronous function form (instead of blk)
    void *ctx;
    bool guest_sync; // guest dispatch_sync: runs after the guest's earlier async items
    uint64_t seq;    // queue order
};

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mq_item *mq_head, **mq_tail = &mq_head;
static uint64_t mq_seq;
atomic_int mainq_pending; // synchronous items queued (callers waiting)

static void mq_unlink(struct mq_item **link, struct mq_item *it) { // mq_lock held
    *link = it->next;
    if (mq_tail == &it->next) mq_tail = link;
    if (it->done) atomic_fetch_sub(&mainq_pending, 1);
}

static void mq_exec(struct mq_item *it) {
    if (it->fn) it->fn(it->ctx);
    else it->blk();
    if (it->done) dispatch_semaphore_signal(it->done);
    else free(it);
}

// Run loop: everything, in order.
void mainq_drain(void) {
    for (;;) {
        pthread_mutex_lock(&mq_lock);
        struct mq_item *it = mq_head;
        if (it) mq_unlink(&mq_head, it);
        pthread_mutex_unlock(&mq_lock);
        if (!it) return;
        mq_exec(it);
    }
}

// Thunk entries: only what a waiting thread needs. Host work (cb_on_main,
// AppKit calls) runs on its own; a guest dispatch_sync takes the guest's
// earlier main-queue items along, in order; other async items stay queued.
void mainq_drain_waiting(void) {
    for (;;) {
        pthread_mutex_lock(&mq_lock);
        struct mq_item **link = &mq_head, *it = mq_head;
        while (it && !it->done) link = &it->next, it = it->next;
        if (!it) {
            pthread_mutex_unlock(&mq_lock);
            return;
        }
        if (!it->guest_sync) {
            mq_unlink(link, it);
            pthread_mutex_unlock(&mq_lock);
            mq_exec(it);
            continue;
        }
        // The head up to and including the guest sync item. By number: a block
        // run here may drain nested and run that item itself, and the item
        // lives on the waiting thread's stack only until it is signalled.
        uint64_t upto = it->seq;
        pthread_mutex_unlock(&mq_lock);
        for (;;) {
            pthread_mutex_lock(&mq_lock);
            struct mq_item *h = mq_head;
            if (!h || h->seq > upto) {
                pthread_mutex_unlock(&mq_lock);
                break;
            }
            mq_unlink(&mq_head, h);
            pthread_mutex_unlock(&mq_lock);
            mq_exec(h);
        }
    }
}

static void mq_push(struct mq_item *it) {
    pthread_mutex_lock(&mq_lock);
    it->seq = ++mq_seq;
    *mq_tail = it;
    mq_tail = &it->next;
    if (it->done) atomic_fetch_add(&mainq_pending, 1);
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

static void mq_run(void (^blk)(void), bool guest) {
    if (pthread_main_np()) {
        blk();
        return;
    }
    struct mq_item it = {.blk = blk, .done = dispatch_semaphore_create(0), .guest_sync = guest};
    mq_push(&it);
    dispatch_semaphore_wait(it.done, DISPATCH_TIME_FOREVER);
    dispatch_release(it.done);
}

void mainq_run_guest(void (^blk)(void)) { mq_run(blk, true); }

void mainq_run(void (^blk)(void)) { mq_run(blk, false); }
