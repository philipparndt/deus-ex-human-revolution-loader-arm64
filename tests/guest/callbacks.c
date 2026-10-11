// Repeated host->guest calls (qsort comparator) and constructors.
#include <stdio.h>
#include <stdlib.h>
static int ctor_runs;
__attribute__((constructor)) static void c1(void) { ctor_runs++; }
__attribute__((constructor)) static void c2(void) { ctor_runs += 10; }
__attribute__((constructor)) static void c3(void) { ctor_runs += 100; }
static int calls;
static int cmp(const void *a, const void *b) { calls++; return *(const int *)a - *(const int *)b; }
int main(void) {
    int v[64];
    for (int i = 0; i < 64; i++) v[i] = (i * 37) % 64;
    qsort(v, 64, sizeof v[0], cmp);
    int ok = 1;
    for (int i = 0; i < 64; i++) ok &= v[i] == i;
    printf("ctors=%d sorted=%d comparator calls=%d\n", ctor_runs, ok, calls);
    return !(ok && ctor_runs == 111);
}
