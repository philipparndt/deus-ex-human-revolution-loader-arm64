// SSE single-precision NaN results (x86 propagation and default NaN). Prints
// the result bits so backends can be compared with real x86 (Rosetta build).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float flt(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static const uint32_t vals[] = {
    0x3f800000, // 1
    0x00000000, // 0
    0x7f800000, // +inf
    0xff800000, // -inf
    0x7fc00001, // QNaN payload 1
    0xffc00002, // -QNaN payload 2
    0x7fa00003, // SNaN payload
    0xbf800000, // -1
};
#define NV (sizeof vals / sizeof vals[0])

int main(void) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < NV; i++)
        for (unsigned j = 0; j < NV; j++) {
            __m128 a = _mm_set_ps(flt(vals[i]), flt(vals[j]), flt(vals[(i + j) % NV]), flt(vals[i]));
            __m128 b = _mm_set_ps(flt(vals[j]), flt(vals[i]), flt(vals[j]), flt(vals[(i * 3 + j) % NV]));
            __m128 r[8] = {_mm_add_ps(a, b), _mm_sub_ps(a, b), _mm_mul_ps(a, b), _mm_div_ps(a, b),
                           _mm_add_ss(a, b), _mm_mul_ss(a, b), _mm_sqrt_ps(a), _mm_sqrt_ss(b)};
            for (int k = 0; k < 8; k++) {
                float o[4];
                _mm_storeu_ps(o, r[k]);
                for (int l = 0; l < 4; l++) {
                    h = (h ^ bits(o[l])) * 16777619u;
                    if (i == 4 && j == 6 && k == 0) printf("qnan+snan lane %d: %08x\n", l, bits(o[l]));
                    if (i == 2 && j == 3 && k == 0) printf("inf+-inf lane %d: %08x\n", l, bits(o[l]));
                }
            }
        }
    // same register as both operands and sqrt in place
    __m128 x = _mm_set_ps(flt(0x7fa00003), -1.0f, 4.0f, flt(0x7fc00001));
    __m128 y = _mm_add_ps(x, x);
    __m128 z = _mm_sqrt_ps(x);
    float o[8];
    _mm_storeu_ps(o, y);
    _mm_storeu_ps(o + 4, z);
    for (int l = 0; l < 8; l++) h = (h ^ bits(o[l])) * 16777619u;
    printf("sse_nan hash %08x\n", h);
    return 0;
}
