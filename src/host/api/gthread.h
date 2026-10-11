// Guest thread records (see pthread.c).
#pragma once
#include "../gmem.h"

#include <pthread.h>
#include <stdbool.h>

#define GTHREAD_MAGIC 0x47544852u // 'GTHR'

struct gthread {
    pthread_t host;
    gaddr_t handle; // guest-visible pthread_t
    gaddr_t stack_lo, stack_hi;
    bool own_stack, detached, started, finished;
    gaddr_t start, arg;
    uint32_t retval;
    uint32_t tls[512];
};

// Record for the calling host thread (created on demand for threads the guest
// did not create, e.g. the main thread or dispatch worker threads).
struct gthread *gthread_current(void);
struct gthread *gthread_from_handle(gaddr_t h);
_Noreturn void gthread_exit_current(uint32_t ret);
