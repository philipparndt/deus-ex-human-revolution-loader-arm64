#pragma once
#include <stdatomic.h>

// Run blk synchronously on the main thread (directly if already there).
void mainq_run(void (^blk)(void));
// Run guest dispatch_sync work on the main thread (FIFO with the guest's async items).
void mainq_run_guest(void (^blk)(void));
// Run all queued work (main run loop).
void mainq_drain(void);
// Run only the work a thread is waiting for (thunk entries on the main thread).
void mainq_drain_waiting(void);
extern atomic_int mainq_pending; // synchronous items waiting (thunk entries drain then)
// Queue fn(ctx) for the main thread without waiting (FIFO with mainq_run).
void mainq_async_f(void *ctx, void (*fn)(void *));
