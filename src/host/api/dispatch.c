// Grand Central Dispatch for the guest: host queues/groups/semaphores behind
// handles; guest blocks and function callbacks run on host worker threads.
#include "blocks.h"
#include "common.h"
#include "../mainq.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>

// The main queue is a data symbol (dispatch_get_main_queue() is a macro taking
// its address); the concurrent queue attribute likewise.
GDATA(_dispatch_main_q, 64, NULL);
GDATA(_dispatch_queue_attr_concurrent, 64, NULL);

static void *dobj(gaddr_t g) {
    static gaddr_t main_q;
    if (!main_q) main_q = hostsym_lookup("__dispatch_main_q");
    if (g == main_q) return (void *)dispatch_get_main_queue();
    return hobj_unwrap(g);
}
#define DQ(i) ((dispatch_queue_t)dobj(ARG(i)))
#define DG(i) ((dispatch_group_t)dobj(ARG(i)))

struct fcall {
    gaddr_t fn, ctx;
};
static void run_fcall(void *p) {
    struct fcall *c = p;
    cpu_thread_attach();
    gcall1(c->fn, c->ctx);
    free(c);
}
static struct fcall *fcall_new(gaddr_t fn, gaddr_t ctx) {
    struct fcall *c = malloc(sizeof *c);
    c->fn = fn;
    c->ctx = ctx;
    return c;
}

static void run_block(void *p) {
    gaddr_t b = (gaddr_t)(uintptr_t)p;
    cpu_thread_attach();
    gblock_invoke0(b);
    gblock_release(b);
}
#define BLOCK_CTX(g) ((void *)(uintptr_t)gblock_copy(g))

THUNK(dispatch_get_global_queue) {
    RET(hobj_wrap(dispatch_get_global_queue((long)ARGI(0), ARG(1))));
}
THUNK(dispatch_queue_create) {
    gaddr_t attr = ARG(1);
    dispatch_queue_attr_t a = NULL;
    if (attr && attr == hostsym_lookup("__dispatch_queue_attr_concurrent")) a = DISPATCH_QUEUE_CONCURRENT;
    RET(hobj_wrap(dispatch_queue_create(ARG(0) ? ARGS(0) : NULL, a)));
}
THUNK(dispatch_get_current_queue) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    dispatch_queue_t q = dispatch_get_current_queue();
#pragma clang diagnostic pop
    RET(q == dispatch_get_main_queue() ? hostsym_lookup("__dispatch_main_q") : hobj_wrap(q));
}
THUNK(dispatch_retain) { dispatch_retain((dispatch_object_t)(dispatch_queue_t)dobj(ARG(0))); }
THUNK(dispatch_release) { dispatch_release((dispatch_object_t)(dispatch_queue_t)dobj(ARG(0))); }

// The guest's main thread may busy-wait without returning to the run loop
// (Feral's window loop), so main-queue work goes through the main-thread work
// queue, which is also drained whenever the main thread calls into the host.
static void q_async(dispatch_queue_t q, void *ctx, void (*fn)(void *)) {
    if (q == dispatch_get_main_queue()) mainq_async_f(ctx, fn);
    else dispatch_async_f(q, ctx, fn);
}
static void q_sync(dispatch_queue_t q, void *ctx, void (*fn)(void *)) {
    if (q == dispatch_get_main_queue()) {
        extern void cgl_drop_held_locks(void), cgl_retake_held_locks(void);
        cgl_drop_held_locks();
        mainq_run(^{
          fn(ctx);
        });
        cgl_retake_held_locks();
    } else {
        dispatch_sync_f(q, ctx, fn);
    }
}

THUNK(dispatch_async) { q_async(DQ(0), BLOCK_CTX(ARG(1)), run_block); }
THUNK(dispatch_sync) { q_sync(DQ(0), BLOCK_CTX(ARG(1)), run_block); }
THUNK(dispatch_async_f) { q_async(DQ(0), fcall_new(ARG(2), ARG(1)), run_fcall); }
THUNK(dispatch_sync_f) { q_sync(DQ(0), fcall_new(ARG(2), ARG(1)), run_fcall); }
THUNK(dispatch_barrier_async) { dispatch_barrier_async_f(DQ(0), BLOCK_CTX(ARG(1)), run_block); }
THUNK(dispatch_barrier_sync) { dispatch_barrier_sync_f(DQ(0), BLOCK_CTX(ARG(1)), run_block); }
// dispatch_after(dispatch_time_t when (64-bit), queue, block)
THUNK(dispatch_after) { dispatch_after_f(ARG64(0), DQ(2), BLOCK_CTX(ARG(3)), run_block); }
THUNK(dispatch_after_f) { dispatch_after_f(ARG64(0), DQ(2), fcall_new(ARG(4), ARG(3)), run_fcall); }

struct apply_ctx {
    gaddr_t fn_or_block, ctx;
    bool is_block;
};
static void run_apply(void *p, size_t i) {
    struct apply_ctx *a = p;
    cpu_thread_attach();
    if (a->is_block) gcall2(gread32(a->fn_or_block + 12), a->fn_or_block, (uint32_t)i);
    else gcall2(a->fn_or_block, a->ctx, (uint32_t)i);
}
THUNK(dispatch_apply) {
    struct apply_ctx a = {ARG(2), 0, true};
    dispatch_apply_f(ARG(0), DQ(1), &a, run_apply);
}
THUNK(dispatch_apply_f) {
    struct apply_ctx a = {ARG(3), ARG(2), false};
    dispatch_apply_f(ARG(0), DQ(1), &a, run_apply);
}

THUNK(dispatch_group_create) { RET(hobj_wrap(dispatch_group_create())); }
THUNK(dispatch_group_async) { dispatch_group_async_f(DG(0), DQ(1), BLOCK_CTX(ARG(2)), run_block); }
THUNK(dispatch_group_async_f) { dispatch_group_async_f(DG(0), DQ(1), fcall_new(ARG(3), ARG(2)), run_fcall); }
THUNK(dispatch_group_wait) { RET(dispatch_group_wait(DG(0), ARG64(1))); }
THUNK(dispatch_group_enter) { dispatch_group_enter(DG(0)); }
THUNK(dispatch_group_leave) { dispatch_group_leave(DG(0)); }
THUNK(dispatch_group_notify) { dispatch_group_notify_f(DG(0), DQ(1), BLOCK_CTX(ARG(2)), run_block); }

THUNK(dispatch_semaphore_create) { RET(hobj_wrap(dispatch_semaphore_create((long)ARGI(0)))); }
THUNK(dispatch_semaphore_signal) { RET(dispatch_semaphore_signal((dispatch_semaphore_t)dobj(ARG(0)))); }
THUNK(dispatch_semaphore_wait) { RET(dispatch_semaphore_wait((dispatch_semaphore_t)dobj(ARG(0)), ARG64(1))); }

// dispatch_time(dispatch_time_t when, int64_t delta) -> dispatch_time_t
THUNK(dispatch_time) { RET64(dispatch_time(ARG64(0), (int64_t)ARG64(2))); }
THUNK(dispatch_walltime) { RET64(dispatch_walltime(NULL, (int64_t)ARG64(1))); }

// dispatch_once(predicate*, block): predicate is a 32-bit long in the guest.
static pthread_mutex_t once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
THUNK(dispatch_once) {
    uint32_t *pred = ARGP(0);
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == 0xFFFFFFFFu) return;
    pthread_mutex_lock(&once_lock);
    if (*pred != 0xFFFFFFFFu) {
        gblock_invoke0(ARG(1));
        __atomic_store_n(pred, 0xFFFFFFFFu, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
}
THUNK(dispatch_once_f) {
    uint32_t *pred = ARGP(0);
    if (__atomic_load_n(pred, __ATOMIC_ACQUIRE) == 0xFFFFFFFFu) return;
    pthread_mutex_lock(&once_lock);
    if (*pred != 0xFFFFFFFFu) {
        gcall1(ARG(2), ARG(1));
        __atomic_store_n(pred, 0xFFFFFFFFu, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
}
