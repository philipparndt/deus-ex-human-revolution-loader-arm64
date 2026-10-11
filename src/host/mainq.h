#pragma once
#include <stdatomic.h>

// Run blk synchronously on the main thread (directly if already there).
void mainq_run(void (^blk)(void));
// Run queued work; called on the main thread.
void mainq_drain(void);
extern atomic_int mainq_pending;
// Queue fn(ctx) for the main thread without waiting (FIFO with mainq_run).
void mainq_async_f(void *ctx, void (*fn)(void *));
