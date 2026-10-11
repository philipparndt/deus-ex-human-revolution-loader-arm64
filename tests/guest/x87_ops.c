// x87 loads/stores/exchange/integer conversion: results and IE/SF status bits for
// edge values and all rounding modes. Prints a hash to compare backends with
// real x87 (Rosetta). Values only pass through loads and stores, so the 80-bit
// registers of real hardware give the same results as the engines' f64.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t h = 2166136261u;
static void mix(uint32_t v) { h = (h ^ v) * 16777619u; }
static void mix64(uint64_t v) { mix((uint32_t)v); mix((uint32_t)(v >> 32)); }
#define SWMASK 0x41 /* IE, SF */

static uint16_t sw(void) { uint16_t s; __asm__ volatile("fnstsw %0" : "=m"(s)); return s; }
static void clex(void) { __asm__ volatile("fnclex"); }
static void setrc(int rc) {
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (uint16_t)((cw & ~0x0c00) | (rc << 10));
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

static const uint64_t d[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0x3fb999999999999aull /* 0.1 */,
                             0x0000000000000001ull /* denormal */, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                             0x7ff8000000000001ull /* QNaN */, 0x7ff0000000000002ull /* SNaN */,
                             0x41dfffffffc00000ull /* 2147483647 */, 0x41e0000000000000ull /* 2^31 */,
                             0xc1e0000000000000ull /* -2^31 */, 0x40e0000800000000ull /* 32768.25 */,
                             0x400c000000000000ull /* 3.5 */, 0xc004000000000000ull /* -2.5 */,
                             0x47efffffe0000000ull /* FLT_MAX */, 0x7fefffffffffffffull /* DBL_MAX */,
                             0x36a0000000000000ull /* tiny */};
static const uint32_t f[] = {0, 0x80000000u, 0x3f800000u, 0x7f800000u, 0x00000001u, 0x7fc00001u, 0x7f800002u,
                             0xff800003u, 0x3dcccccdu};
#define N(a) (sizeof a / sizeof a[0])

int main(void) {
    for (int rc = 0; rc < 4; rc++) {
        setrc(rc);
        for (unsigned i = 0; i < N(d); i++) {
            double x; memcpy(&x, &d[i], 8);
            float o32; double o64; int32_t i32 = 0; int16_t i16 = 0; int32_t t32 = 0;
            clex();
            __asm__ volatile("fldl %1; fsts %0; fstpl %2" : "=m"(o32), "=m"(x), "=m"(o64) : : "st");
            uint32_t b32; uint64_t b64; memcpy(&b32, &o32, 4); memcpy(&b64, &o64, 8);
            mix(b32); mix64(b64); mix(sw() & SWMASK);
            clex();
            __asm__ volatile("fldl %2; fistl %0; fistps %1" : "=m"(i32), "=m"(i16) : "m"(x) : "st");
            mix((uint32_t)i32); mix((uint16_t)i16); mix(sw() & SWMASK);
            clex();
            __asm__ volatile("fldl %1; fisttpl %0" : "=m"(t32) : "m"(x) : "st");
            mix((uint32_t)t32); mix(sw() & SWMASK);
        }
    }
    setrc(0);
    for (unsigned i = 0; i < N(f); i++) {
        float x; memcpy(&x, &f[i], 4);
        double o; clex();
        __asm__ volatile("flds %1; fstpl %0" : "=m"(o) : "m"(x) : "st");
        uint64_t b; memcpy(&b, &o, 8); mix64(b); mix(sw() & SWMASK);
    }
    int16_t s16[] = {0, 1, -1, 32767, -32768};
    int32_t s32[] = {0, 7, -7, 2147483647, (int32_t)0x80000000};
    int64_t s64[] = {0, 3, -3, 0x7fffffffffffffffll, (int64_t)0x8000000000000000ull, 0x20000000000001ll};
    for (unsigned i = 0; i < N(s16); i++) { double o; __asm__ volatile("filds %1; fstpl %0" : "=m"(o) : "m"(s16[i]) : "st"); mix64(*(uint64_t *)&o); }
    for (unsigned i = 0; i < N(s32); i++) { double o; __asm__ volatile("fildl %1; fstpl %0" : "=m"(o) : "m"(s32[i]) : "st"); mix64(*(uint64_t *)&o); }
    for (unsigned i = 0; i < N(s64); i++) { double o; __asm__ volatile("fildll %1; fstpl %0" : "=m"(o) : "m"(s64[i]) : "st"); mix64(*(uint64_t *)&o); }
    /* register forms: fld st(i), fxch, fst st(i), fstp st(0), fldz/fld1 */
    {
        double a = 1.5, b = -2.25, o[4];
        __asm__ volatile("fldl %4; fldl %5; fld %%st(1); fxch %%st(2); fst %%st(1); fldz; fld1; faddp; fstpl %0; fstpl %1; fstpl %2; fstp %%st(0); fldl %4; fstpl %3"
                         : "=m"(o[0]), "=m"(o[1]), "=m"(o[2]), "=m"(o[3]) : "m"(a), "m"(b) : "st", "st(1)", "st(2)", "st(3)");
        for (int k = 0; k < 4; k++) mix64(*(uint64_t *)&o[k]);
    }
    /* stack overflow (9 pushes) and underflow (pop of an empty register) set IE+SF */
    {
        double one = 1.0, o; clex();
        __asm__ volatile("fld1; fld1; fld1; fld1; fld1; fld1; fld1; fld1; fldl %0" : : "m"(one));
        mix(sw() & SWMASK);
        __asm__ volatile("emms"); clex();
        __asm__ volatile("fstpl %0" : "=m"(o));
        mix(sw() & SWMASK);
        __asm__ volatile("emms"); __asm__ volatile("fninit");
    }
    printf("x87_ops hash %08x\n", h);
    return 0;
}
