// Locked instructions under contention. Feral's mutex decides ownership from the
// old value cmpxchg leaves in eax (not from ZF); a CAS retry inside the engine
// must not report a stale success there.
#include <pthread.h>
#include <stdio.h>

#include "libc_check.h"

#define THREADS 4
#define ITERS 2000000

static volatile int lock;
static volatile int inside, counter, overlap;
static volatile int xcount, ready;

// Old value via eax only, ignoring ZF (as Feral's lock does).
static int cas_eax(volatile int *p, int expected, int desired) {
    int old = expected;
    __asm__ volatile("lock cmpxchgl %2, %1" : "+a"(old), "+m"(*p) : "r"(desired) : "cc", "memory");
    return old;
}

static void *worker(void *p) {
    (void)p;
    __sync_fetch_and_add(&ready, 1);
    while (ready < THREADS) {
    }
    for (int i = 0; i < ITERS; i++) {
        while (cas_eax(&lock, 0, 1) != 0) {
        }
        if (++inside != 1) overlap++;
        counter++;
        inside--;
        __sync_lock_release(&lock);
        __sync_fetch_and_add(&xcount, 1);
    }
    return NULL;
}

int main(void) {
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    CHECK(overlap == 0, "mutual exclusion (%d overlaps)", overlap);
    CHECK(counter == THREADS * ITERS, "counter %d", counter);
    CHECK(xcount == THREADS * ITERS, "xadd count %d", xcount);
    DONE();
}
