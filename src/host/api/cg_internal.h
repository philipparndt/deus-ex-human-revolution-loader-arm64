// Shared helpers for the CoreGraphics / CoreText / ImageIO thunks.
//
// i386 ABI reminders: CGFloat is float (4 bytes). CGPoint/CGSize are 8 bytes
// (two float stack slots in, edx:eax out), CGRect is 16 bytes (four slots in,
// hidden-pointer return), CGAffineTransform is 24 bytes (six slots in,
// hidden-pointer return). Float results come back in st(0) (RETD).
#pragma once
#include "cf_bridge.h"

#include <ApplicationServices/ApplicationServices.h>

__asm__(".linker_option \"-framework\", \"ApplicationServices\"");

static inline uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline float u2f(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline float greadf(gaddr_t a) { return u2f(gread32(a)); }
static inline void gwritef(gaddr_t a, CGFloat v) { gwrite32(a, f2u((float)v)); }

#define ARGPOINT(i) CGPointMake(ARGF(i), ARGF((i) + 1))
#define ARGSIZE(i) CGSizeMake(ARGF(i), ARGF((i) + 1))
#define ARGRECT(i) CGRectMake(ARGF(i), ARGF((i) + 1), ARGF((i) + 2), ARGF((i) + 3))
#define ARGXFORM(i)                                                                                \
    ((CGAffineTransform){ARGF(i), ARGF((i) + 1), ARGF((i) + 2), ARGF((i) + 3), ARGF((i) + 4), ARGF((i) + 5)})

// 8-byte float pair returned in edx:eax.
#define RET_FPAIR(a, b) RET64((uint64_t)f2u((float)(a)) | ((uint64_t)f2u((float)(b)) << 32))
#define RET_POINT(p) RET_FPAIR((p).x, (p).y)
#define RET_SIZE(s) RET_FPAIR((s).width, (s).height)

static inline void put_grect(gaddr_t g, CGRect rc) {
    gwritef(g, rc.origin.x);
    gwritef(g + 4, rc.origin.y);
    gwritef(g + 8, rc.size.width);
    gwritef(g + 12, rc.size.height);
}
static inline CGRect get_grect(gaddr_t g) {
    return CGRectMake(greadf(g), greadf(g + 4), greadf(g + 8), greadf(g + 12));
}
static inline void put_gsize(gaddr_t g, CGSize s) {
    gwritef(g, s.width);
    gwritef(g + 4, s.height);
}
static inline void put_gxform(gaddr_t g, CGAffineTransform t) {
    gwritef(g, t.a);
    gwritef(g + 4, t.b);
    gwritef(g + 8, t.c);
    gwritef(g + 12, t.d);
    gwritef(g + 16, t.tx);
    gwritef(g + 20, t.ty);
}
static inline CGAffineTransform get_gxform(gaddr_t g) {
    return (CGAffineTransform){greadf(g), greadf(g + 4), greadf(g + 8), greadf(g + 12), greadf(g + 16), greadf(g + 20)};
}

// Struct returns through a hidden first argument (callee pops it).
#define RET_RECT(rc)                                                                               \
    do {                                                                                           \
        put_grect(ARG(0), (rc));                                                                   \
        r->pop = 4;                                                                                \
        RET(ARG(0));                                                                               \
    } while (0)
#define RET_XFORM(t)                                                                               \
    do {                                                                                           \
        put_gxform(ARG(0), (t));                                                                   \
        r->pop = 4;                                                                                \
        RET(ARG(0));                                                                               \
    } while (0)
