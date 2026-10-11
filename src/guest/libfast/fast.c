// Guest-side (i386) versions of hot libc functions. Each call into the host
// layer costs a 32/64-bit mode switch (~100 ns under Rosetta); these run
// entirely in guest code. Linked as part of the preloaded guest runtime, whose
// exports take precedence over host thunks.
#include <stddef.h>
#include <stdint.h>

#define EXPORT __attribute__((visibility("default"), used))

EXPORT int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n >= 4) {
        uint32_t x, y;
        __builtin_memcpy(&x, p, 4);
        __builtin_memcpy(&y, q, 4);
        if (x != y) break;
        p += 4, q += 4, n -= 4;
    }
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

// rep movs / rep stos: fast on modern CPUs and under Rosetta/our JIT.
EXPORT void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

EXPORT void *memmove(void *dst, const void *src, size_t n) {
    if ((uintptr_t)dst - (uintptr_t)src >= n) return memcpy(dst, src, n); // no harmful overlap
    // backwards copy
    void *d = (uint8_t *)dst + n - 1;
    const void *s = (const uint8_t *)src + n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

EXPORT void *memset(void *dst, int c, size_t n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

EXPORT void bzero(void *dst, size_t n) { memset(dst, 0, n); }
EXPORT void __bzero(void *dst, size_t n) { memset(dst, 0, n); }

EXPORT size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

EXPORT int strcmp(const char *a, const char *b) {
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while (*p && *p == *q) p++, q++;
    return *p - *q;
}

EXPORT int strncmp(const char *a, const char *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--, p++, q++) {
        if (*p != *q) return *p - *q;
        if (!*p) return 0;
    }
    return 0;
}

// <math.h> classification helpers (Darwin's i386 macros call these)
EXPORT int __isnanf(float x) { return x != x; }
EXPORT int __isnand(double x) { return x != x; }
EXPORT int __isfinitef(float x) { return __builtin_isfinite(x); }
EXPORT int __isfinited(double x) { return __builtin_isfinite(x); }
EXPORT int __isinff(float x) { return __builtin_isinf(x); }
EXPORT int __isinfd(double x) { return __builtin_isinf(x); }
EXPORT int __signbitf(float x) { return __builtin_signbit(x); }
EXPORT int __signbitd(double x) { return __builtin_signbit(x); }

// 64-bit division helpers (normally from libgcc/compiler-rt). Uses the i386
// 64/32 divide when the divisor fits; otherwise shift-subtract.
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    if (!(d >> 32)) {
        uint32_t dl = (uint32_t)d, hi = (uint32_t)(n >> 32), lo = (uint32_t)n, qh = 0, r;
        if (hi >= dl) {
            qh = hi / dl;
            hi = hi % dl;
        }
        uint32_t ql;
        __asm__("divl %4" : "=a"(ql), "=d"(r) : "a"(lo), "d"(hi), "rm"(dl));
        if (rem) *rem = r;
        return ((uint64_t)qh << 32) | ql;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

EXPORT uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
EXPORT uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r;
    udivmod64(n, d, &r);
    return r;
}
EXPORT int64_t __divdi3(int64_t n, int64_t d) {
    uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n, ud = d < 0 ? -(uint64_t)d : (uint64_t)d;
    uint64_t q = udivmod64(un, ud, 0);
    return (n < 0) != (d < 0) ? -(int64_t)q : (int64_t)q;
}
EXPORT int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n, ud = d < 0 ? -(uint64_t)d : (uint64_t)d, r;
    udivmod64(un, ud, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
