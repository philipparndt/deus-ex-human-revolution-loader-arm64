// CoreVideo: pixel buffers (contents mirrored into guest memory while locked)
// and CVDisplayLink (vsync callback on a host thread calling into the guest).
//
// CVTimeStamp (80 bytes), CVSMPTETime (24) and CVTime (16) have identical
// layouts on i386 and x86_64 (verified with offsetof), so they are copied raw.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include "ret_return.h"

__asm__(".linker_option \"-framework\", \"CoreVideo\"");

_Static_assert(sizeof(CVTimeStamp) == 80, "CVTimeStamp layout");
_Static_assert(sizeof(CVTime) == 16, "CVTime layout");

static void init_pf_key(gaddr_t a) { gwrite32(a, cfg(kCVPixelBufferPixelFormatTypeKey)); }
GDATA(kCVPixelBufferPixelFormatTypeKey, 4, init_pf_key);
static void init_w_key(gaddr_t a) { gwrite32(a, cfg(kCVPixelBufferWidthKey)); }
GDATA(kCVPixelBufferWidthKey, 4, init_w_key);
static void init_h_key(gaddr_t a) { gwrite32(a, cfg(kCVPixelBufferHeightKey)); }
GDATA(kCVPixelBufferHeightKey, 4, init_h_key);
static void init_gl_key(gaddr_t a) { gwrite32(a, cfg(kCVPixelBufferOpenGLCompatibilityKey)); }
GDATA(kCVPixelBufferOpenGLCompatibilityKey, 4, init_gl_key);

// --- pixel buffers ------------------------------------------------------------------------------

struct pbmirror {
    gaddr_t buf;
    uint32_t cap, size;
    int locks;
    CVPixelBufferLockFlags flags;
};
static char pb_key;
static void pb_free(void *p) {
    struct pbmirror *m = p;
    if (m->buf) gheap_free(m->buf);
    free(m);
}
static struct pbmirror *pb_mirror(CVPixelBufferRef pb) {
    struct pbmirror *m = cf_attached(pb, &pb_key);
    if (!m) {
        m = calloc(1, sizeof *m);
        cf_attach(pb, &pb_key, m, pb_free);
    }
    return m;
}

THUNK(CVPixelBufferGetTypeID) { RET(CVPixelBufferGetTypeID()); }
THUNK(CVPixelBufferGetWidth) { RET(CVPixelBufferGetWidth((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferGetHeight) { RET(CVPixelBufferGetHeight((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferGetBytesPerRow) { RET(CVPixelBufferGetBytesPerRow((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferGetPixelFormatType) { RET(CVPixelBufferGetPixelFormatType((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferGetDataSize) { RET(CVPixelBufferGetDataSize((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferIsPlanar) { RET_BOOL(CVPixelBufferIsPlanar((CVPixelBufferRef)ARGCF(0))); }

static uint32_t pb_size(CVPixelBufferRef pb) {
    size_t s = CVPixelBufferGetBytesPerRow(pb) * CVPixelBufferGetHeight(pb);
    size_t ds = CVPixelBufferGetDataSize(pb);
    if (ds > s) s = ds;
    return (uint32_t)s;
}

THUNK(CVPixelBufferLockBaseAddress) {
    CVPixelBufferRef pb = (CVPixelBufferRef)ARGCF(0);
    CVReturn rc = CVPixelBufferLockBaseAddress(pb, ARG(1));
    if (rc != kCVReturnSuccess) RET(rc);
    struct pbmirror *m = pb_mirror(pb);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (m->locks++ == 0 && base && !host_ptr_in_guest(base)) {
        // The mirror only lives while locked (the guest may only use the base
        // address then); movie frames would otherwise pin guest memory per buffer.
        uint32_t size = pb_size(pb);
        m->buf = gheap_alloc(size);
        if (!m->buf) {
            LOGE("cv: out of guest memory for a %u byte pixel buffer", size);
            m->locks--;
            CVPixelBufferUnlockBaseAddress(pb, ARG(1));
            RET(kCVReturnAllocationFailed);
        }
        m->cap = m->size = size;
        m->flags = ARG(1);
        memcpy(g2h(m->buf), base, size);
    }
    RET(rc);
}

THUNK(CVPixelBufferUnlockBaseAddress) {
    CVPixelBufferRef pb = (CVPixelBufferRef)ARGCF(0);
    struct pbmirror *m = pb_mirror(pb);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (m->locks > 0 && --m->locks == 0 && base && !host_ptr_in_guest(base) && m->buf &&
        !(m->flags & kCVPixelBufferLock_ReadOnly))
        memcpy(base, g2h(m->buf), m->size); // write back guest modifications
    if (m->locks == 0 && m->buf) {
        gheap_free(m->buf);
        m->buf = 0;
        m->cap = 0;
    }
    RET(CVPixelBufferUnlockBaseAddress(pb, ARG(1)));
}

THUNK(CVPixelBufferGetBaseAddress) {
    CVPixelBufferRef pb = (CVPixelBufferRef)ARGCF(0);
    void *base = CVPixelBufferGetBaseAddress(pb);
    if (!base) RET(0);
    if (host_ptr_in_guest(base)) RET(h2g(base));
    struct pbmirror *m = pb_mirror(pb);
    if (!m->locks || !m->buf) {
        LOGE("cv: CVPixelBufferGetBaseAddress on unlocked buffer");
        RET(0);
    }
    RET(m->buf);
}

THUNK(CVPixelBufferRetain) { RET_CF(CVPixelBufferRetain((CVPixelBufferRef)ARGCF(0))); }
THUNK(CVPixelBufferRelease) { CVPixelBufferRelease((CVPixelBufferRef)ARGCF(0)); }
THUNK(CVBufferRetain) { RET_CF(CVBufferRetain((CVBufferRef)ARGCF(0))); }
THUNK(CVBufferRelease) { CVBufferRelease((CVBufferRef)ARGCF(0)); }

// --- display link -------------------------------------------------------------------------------

struct gdl {
    gaddr_t fn, ctx;
    gaddr_t ts; // guest buffer: now (80) + output time (80) + flagsOut (8)
};
static char dl_key;
static void gdl_free(void *p) {
    struct gdl *g = p;
    if (g->ts) gheap_free(g->ts);
    free(g);
}

static CVReturn dl_cb(CVDisplayLinkRef link, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags flagsIn,
                      CVOptionFlags *flagsOut, void *ctx) {
    struct gdl *g = ctx;
    memcpy(g2h(g->ts), now, 80);
    memcpy(g2h(g->ts + 80), out, 80);
    uint64_t fo = flagsOut ? *flagsOut : 0;
    gwrite32(g->ts + 160, (uint32_t)fo);
    gwrite32(g->ts + 164, (uint32_t)(fo >> 32));
    // CVOptionFlags is uint64_t: flagsIn takes two stack slots.
    uint32_t a[7] = {cfg(link), g->ts, g->ts + 80, (uint32_t)flagsIn, (uint32_t)(flagsIn >> 32), g->ts + 160, g->ctx};
    CVReturn rc = (CVReturn)cpu_call(g->fn, 7, a);
    if (flagsOut) *flagsOut = gread32(g->ts + 160) | (uint64_t)gread32(g->ts + 164) << 32;
    return rc;
}

static void put_link(gaddr_t out, CVReturn rc, CVDisplayLinkRef l) { gwrite32(out, rc == kCVReturnSuccess ? cfg(l) : 0); }

THUNK(CVDisplayLinkCreateWithActiveCGDisplays) {
    CVDisplayLinkRef l = NULL;
    CVReturn rc = CVDisplayLinkCreateWithActiveCGDisplays(&l);
    put_link(ARG(0), rc, l);
    RET(rc);
}
THUNK(CVDisplayLinkCreateWithCGDisplay) {
    CVDisplayLinkRef l = NULL;
    CVReturn rc = CVDisplayLinkCreateWithCGDisplay(ARG(0), &l);
    put_link(ARG(1), rc, l);
    RET(rc);
}
// CVDisplayLinkCreateWithCGDisplays(CGDirectDisplayID *displays, CFIndex count, CVDisplayLinkRef *out)
THUNK(CVDisplayLinkCreateWithCGDisplays) {
    CVDisplayLinkRef l = NULL;
    CVReturn rc = CVDisplayLinkCreateWithCGDisplays(ARGP(0), ARGIDX(1), &l);
    put_link(ARG(2), rc, l);
    RET(rc);
}
THUNK(CVDisplayLinkSetOutputCallback) {
    CVDisplayLinkRef l = (CVDisplayLinkRef)ARGCF(0);
    if (!ARG(1)) {
        CVReturn rc = CVDisplayLinkSetOutputCallback(l, NULL, NULL);
        cf_attach(l, &dl_key, NULL, NULL);
        RET(rc);
    }
    struct gdl *g = calloc(1, sizeof *g);
    g->fn = ARG(1);
    g->ctx = ARG(2);
    g->ts = gheap_calloc(1, 176);
    CVReturn rc = CVDisplayLinkSetOutputCallback(l, dl_cb, g);
    cf_attach(l, &dl_key, g, gdl_free);
    RET(rc);
}
THUNK(CVDisplayLinkStart) { RET(CVDisplayLinkStart((CVDisplayLinkRef)ARGCF(0))); }
THUNK(CVDisplayLinkStop) { RET(CVDisplayLinkStop((CVDisplayLinkRef)ARGCF(0))); }
THUNK(CVDisplayLinkIsRunning) { RET_BOOL(CVDisplayLinkIsRunning((CVDisplayLinkRef)ARGCF(0))); }
THUNK(CVDisplayLinkRetain) { RET_CF(CVDisplayLinkRetain((CVDisplayLinkRef)ARGCF(0))); }
THUNK(CVDisplayLinkRelease) { CVDisplayLinkRelease((CVDisplayLinkRef)ARGCF(0)); }
THUNK(CVDisplayLinkGetTypeID) { RET(CVDisplayLinkGetTypeID()); }
THUNK(CVDisplayLinkSetCurrentCGDisplay) { RET(CVDisplayLinkSetCurrentCGDisplay((CVDisplayLinkRef)ARGCF(0), ARG(1))); }
THUNK(CVDisplayLinkGetCurrentCGDisplay) { RET(CVDisplayLinkGetCurrentCGDisplay((CVDisplayLinkRef)ARGCF(0))); }
// The guest's CGL objects belong to the GL layer; use the main display.
THUNK(CVDisplayLinkSetCurrentCGDisplayFromOpenGLContext) {
    RET(CVDisplayLinkSetCurrentCGDisplay((CVDisplayLinkRef)ARGCF(0), CGMainDisplayID()));
}
// CVTime CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link): 16-byte struct via hidden pointer.
THUNK(CVDisplayLinkGetNominalOutputVideoRefreshPeriod) {
    CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod((CVDisplayLinkRef)ARGCF(1));
    memcpy(ARGP(0), &t, sizeof t);
    r->pop = 4;
    RET(ARG(0));
}
THUNK(CVDisplayLinkGetOutputVideoLatency) {
    CVTime t = CVDisplayLinkGetOutputVideoLatency((CVDisplayLinkRef)ARGCF(1));
    memcpy(ARGP(0), &t, sizeof t);
    r->pop = 4;
    RET(ARG(0));
}
THUNK(CVDisplayLinkGetActualOutputVideoRefreshPeriod) {
    RETD(CVDisplayLinkGetActualOutputVideoRefreshPeriod((CVDisplayLinkRef)ARGCF(0)));
}
THUNK(CVDisplayLinkGetCurrentTime) {
    CVTimeStamp ts;
    memcpy(&ts, ARGP(1), sizeof ts); // version field is an input
    CVReturn rc = CVDisplayLinkGetCurrentTime((CVDisplayLinkRef)ARGCF(0), &ts);
    memcpy(ARGP(1), &ts, sizeof ts);
    RET(rc);
}
THUNK(CVGetCurrentHostTime) { RET64(CVGetCurrentHostTime()); }
THUNK(CVGetHostClockFrequency) { RETD(CVGetHostClockFrequency()); }
