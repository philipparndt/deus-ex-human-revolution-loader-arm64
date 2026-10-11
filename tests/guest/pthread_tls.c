// Guest-side pthread_self/getspecific/setspecific (libfast + host stack table).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

#include "libc_check.h"

static pthread_key_t key;
static volatile int destructed;
static void dtor(void *v) { if ((intptr_t)v == 77) __sync_fetch_and_add(&destructed, 1); } // threads exit concurrently
static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }

struct arg { pthread_t self_seen; int ok; int n; };
static void *worker(void *p) {
    struct arg *a = p;
    a->self_seen = pthread_self();
    a->ok = pthread_getspecific(key) == NULL; // fresh thread: empty slot
    pthread_setspecific(key, (void *)(intptr_t)(1000 + a->n));
    for (int i = 0; i < 10000; i++)
        if (pthread_getspecific(key) != (void *)(intptr_t)(1000 + a->n)) a->ok = 0;
    pthread_setspecific(key, (void *)77); // destructor value
    return NULL;
}

int main(void) {
    CHECK(pthread_key_create(&key, dtor) == 0, "key_create");
    pthread_setspecific(key, (void *)42);
    pthread_t main_self = pthread_self();
    CHECK(pthread_equal(main_self, pthread_self()), "self stable");
    pthread_t th[4];
    struct arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].n = i;
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 4; i++) {
        CHECK(pthread_equal(args[i].self_seen, th[i]), "thread %d self == create handle", i);
        CHECK(args[i].ok, "thread %d tls isolated", i);
        CHECK(!pthread_equal(args[i].self_seen, main_self), "thread %d differs from main", i);
    }
    CHECK(pthread_getspecific(key) == (void *)42, "main tls untouched");
    CHECK(destructed == 4, "destructors ran (%d)", destructed);
    const int N = 2000000;
    double t0 = now();
    volatile uintptr_t sink = 0;
    for (int i = 0; i < N; i++) sink += (uintptr_t)pthread_self();
    double t1 = now();
    for (int i = 0; i < N; i++) sink += (uintptr_t)pthread_getspecific(key);
    double t2 = now();
    printf("pthread_self: %.1f ns/call, pthread_getspecific: %.1f ns/call\n", (t1 - t0) / N * 1e9, (t2 - t1) / N * 1e9);
    DONE();
}
