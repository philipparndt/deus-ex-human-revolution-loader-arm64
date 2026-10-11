// CPU benchmark for comparing backends (integer, float/SSE, memory).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }

typedef struct { float m[16]; } mat4;
static void mul(mat4 *r, const mat4 *a, const mat4 *b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
            r->m[i * 4 + j] = s;
        }
}

int main(void) {
    double t0 = now();
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 200000000u; i++) h = (h ^ (i & 0xff)) * 16777619u;
    double t1 = now();
    mat4 a, b, c;
    for (int i = 0; i < 16; i++) { a.m[i] = (float)i * 0.01f; b.m[i] = 1.0f - (float)i * 0.02f; }
    for (int n = 0; n < 5000000; n++) { mul(&c, &a, &b); a.m[0] = c.m[5] * 0.5f; }
    double t2 = now();
    static uint8_t buf[1 << 20], buf2[1 << 20];
    uint32_t sum = 0;
    for (int n = 0; n < 400; n++) {
        for (int i = 0; i < (1 << 20); i += 64) buf[i] = (uint8_t)(i + n);
        memcpy(buf2, buf, sizeof buf);
        for (int i = 0; i < (1 << 20); i += 4) sum += buf2[i];
    }
    double t3 = now();
    printf("int %.3fs  float %.3fs  mem %.3fs  total %.3fs  (h=%u c=%f s=%u)\n", t1 - t0, t2 - t1, t3 - t2, t3 - t0, h,
           c.m[3], sum);
    return 0;
}
