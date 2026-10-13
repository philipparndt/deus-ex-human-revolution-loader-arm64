// Memory-ordering litmus tests (x86 TSO) on two threads, plain loads/stores.
//  MP: A: data=1; flag=1   B: r1=flag; r2=data    forbidden on x86: r1=1, r2=0
//  LB: A: r1=x; y=1        B: r2=y; x=1           forbidden on x86: r1=1, r2=1
//  SB: A: x=1; r1=y        B: y=1; r2=x           allowed on x86:  r1=0, r2=0
// Each round uses fresh cache lines; both threads start a round together.
// LITMUS_ROUNDS (default 200000) rounds per test. Prints the counts; with
// LITMUS_STRICT=1 the exit code is 1 if a forbidden outcome was seen (the native
// JIT shows MP violations with DXCPU_TSO=0).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SLOTS 1024
struct slot { volatile int a; char pad1[60]; volatile int b; char pad2[60]; } __attribute__((aligned(64)));
static struct slot s[SLOTS];
static volatile int r1[SLOTS], r2[SLOTS];
static volatile int go, done_a, done_b;
static int test, rounds;

static void spin_until(volatile int *v, int x) { while (*v != x) { } }

static void *thread_a(void *p) {
    for (int round = 1; round <= rounds; round++) {
        spin_until(&go, round);
        struct slot *c = &s[round % SLOTS];
        int k = round % SLOTS;
        if (test == 0) { c->a = 1; c->b = 1; }                 /* MP writer: data, flag */
        else if (test == 1) { r1[k] = c->a; c->b = 1; }        /* LB */
        else { c->a = 1; r1[k] = c->b; }                       /* SB */
        __sync_fetch_and_add(&done_a, 1);
    }
    return p;
}
static void *thread_b(void *p) {
    for (int round = 1; round <= rounds; round++) {
        spin_until(&go, round);
        struct slot *c = &s[round % SLOTS];
        int k = round % SLOTS;
        if (test == 0) { r1[k] = c->b; r2[k] = c->a; }         /* MP reader: flag, data */
        else if (test == 1) { r2[k] = c->b; c->a = 1; }       /* LB */
        else { c->b = 1; r2[k] = c->a; }                       /* SB */
        __sync_fetch_and_add(&done_b, 1);
    }
    return p;
}

int main(void) {
    const char *e = getenv("LITMUS_ROUNDS");
    rounds = e ? atoi(e) : 200000;

    const char *names[] = {"MP", "LB", "SB"};
    int bad = 0;
    for (test = 0; test < 3; test++) {
        long counts[4] = {0};
        go = done_a = done_b = 0;
        for (int i = 0; i < SLOTS; i++) s[i].a = s[i].b = 0;
        pthread_t ta, tb;
        pthread_create(&ta, NULL, thread_a, NULL);
        pthread_create(&tb, NULL, thread_b, NULL);
        for (int round = 1; round <= rounds; round++) {
            struct slot *c = &s[round % SLOTS];
            c->a = c->b = 0;
            __sync_synchronize();
            go = round;
            spin_until(&done_a, round);
            spin_until(&done_b, round);
            int k = round % SLOTS;
            counts[(r1[k] ? 2 : 0) | (r2[k] ? 1 : 0)]++;
        }
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        long forbidden = test == 0 ? counts[2] : test == 1 ? counts[3] : 0;
        printf("%s: 00=%ld 01=%ld 10=%ld 11=%ld  forbidden=%ld%s\n", names[test], counts[0], counts[1], counts[2],
               counts[3], forbidden, test == 2 ? " (00 is allowed on x86)" : "");
        if (forbidden) bad = 1;
    }
    return getenv("LITMUS_STRICT") ? bad : 0;
}
