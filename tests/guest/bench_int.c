// Integer/memory benchmark (no floating point).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }
static uint32_t buf[1 << 18];
int main(void) {
    double t0 = now();
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 200000000u; i++) h = (h ^ (i & 0xff)) * 16777619u;
    double t1 = now();
    uint32_t sum = 0;
    for (int n = 0; n < 200; n++)
        for (int i = 0; i < (1 << 18); i++) { buf[i] = buf[i] * 3 + (uint32_t)i + n; sum += buf[i] >> 3; }
    double t2 = now();
    int calls = 0;
    for (int n = 0; n < 2000000; n++) calls += (int)strlen("deus ex") + n % 3;
    double t3 = now();
    printf("hash %.3fs  array %.3fs  calls %.3fs  (h=%u s=%u c=%d)\n", t1 - t0, t2 - t1, t3 - t2, h, sum, calls);
    return 0;
}
