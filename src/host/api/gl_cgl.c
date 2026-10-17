// CGL (OpenGL.framework context management).
//
// CGL objects (contexts, pixel formats, renderer infos, share groups) are host
// pointers and reach the guest as hobj handles. Attribute lists and GLint
// parameter arrays have the same layout on both sides (32-bit ints).
//
// Drawable attachment (CGLSetSurface, NSOpenGLContext views, AGL) is not done
// here; without a drawable the context renders into FBOs only.
#include <stdlib.h>
#include <time.h>
#include "gl_internal.h"
#include "spikes.h"
#include "gmem.h"

#include <OpenGL/gl3.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdlib.h>

#define CTX(i) ((CGLContextObj)hobj_unwrap(ARG(i)))
#define PIX(i) ((CGLPixelFormatObj)hobj_unwrap(ARG(i)))

// --- pixel formats ------------------------------------------------------------------
static bool pfa_has_value(int a) {
    switch (a) {
    case kCGLPFAAuxBuffers:
    case kCGLPFAColorSize:
    case kCGLPFAAlphaSize:
    case kCGLPFADepthSize:
    case kCGLPFAStencilSize:
    case kCGLPFAAccumSize:
    case kCGLPFASampleBuffers:
    case kCGLPFASamples:
    case kCGLPFARendererID:
    case kCGLPFADisplayMask:
    case kCGLPFAOpenGLProfile:
    case kCGLPFAVirtualScreenCount:
        return true;
    default:
        return false;
    }
}

// Retry level at which an attribute is dropped (0 = always kept).
static int pfa_drop_level(int a) {
    switch (a) {
    // Removed from modern macOS or tied to obsolete hardware/display setups.
    case 6: /* Stereo */
    case 53: /* OffScreen */
    case 54: /* FullScreen */
    case 71: /* SingleRenderer */
    case 75: /* Robust */
    case 78: /* MPSafe */
    case 80: /* Window */
    case 81: /* MultiScreen */
    case 83: /* Compliant */
    case 90: /* PBuffer */
    case 91: /* RemotePBuffer */
    case kCGLPFAAuxBuffers:
    case kCGLPFAAccumSize:
    case kCGLPFAAuxDepthStencil:
    case kCGLPFARendererID:
    case kCGLPFADisplayMask:
        return 1;
    // Optional quality / policy requests.
    case kCGLPFASampleBuffers:
    case kCGLPFASamples:
    case kCGLPFAMultisample:
    case kCGLPFASupersample:
    case kCGLPFASampleAlpha:
    case kCGLPFAColorFloat:
    case kCGLPFAMinimumPolicy:
    case kCGLPFAMaximumPolicy:
    case kCGLPFAClosestPolicy:
    case kCGLPFANoRecovery:
    case kCGLPFATripleBuffer:
    case kCGLPFABackingStore:
    case kCGLPFABackingVolatile:
    case kCGLPFAVirtualScreenCount:
        return 2;
    // Last resort keeps only profile, acceleration and double buffering.
    case kCGLPFAColorSize:
    case kCGLPFAAlphaSize:
    case kCGLPFADepthSize:
    case kCGLPFAStencilSize:
    case kCGLPFAAcceleratedCompute:
        return 3;
    default:
        return 0;
    }
}

// Optional override: DXH_GL_PROFILE=legacy|3.2|4.1
static int profile_override(void) {
    const char *e = getenv("DXH_GL_PROFILE");
    if (!e || !*e) return 0;
    if (!strcmp(e, "legacy")) return kCGLOGLPVersion_Legacy;
    if (!strcmp(e, "3.2")) return kCGLOGLPVersion_3_2_Core;
    if (!strcmp(e, "4.1")) return kCGLOGLPVersion_GL4_Core;
    LOGE("cgl: ignoring DXH_GL_PROFILE=%s", e);
    return 0;
}

THUNK(CGLChoosePixelFormat) {
    gaddr_t attrs = ARG(0);
    // Read the guest list (pairs for valued attributes, 0-terminated).
    int in[128], nin = 0;
    char desc[1024];
    size_t dl = 0;
    desc[0] = 0;
    for (gaddr_t p = attrs; p && nin < 126; p += 4) {
        int a = (int)gread32(p);
        if (!a) break;
        in[nin++] = a;
        if (pfa_has_value(a)) {
            p += 4;
            in[nin++] = (int)gread32(p);
            dl += (size_t)snprintf(desc + dl, dl < sizeof desc ? sizeof desc - dl : 0, " %d=%#x", a, in[nin - 1]);
        } else {
            dl += (size_t)snprintf(desc + dl, dl < sizeof desc ? sizeof desc - dl : 0, " %d", a);
        }
        if (dl >= sizeof desc) dl = sizeof desc - 1;
    }
    LOGI("cgl: CGLChoosePixelFormat attributes:%s", desc);
    int prof = profile_override();

    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError err = kCGLBadAttribute;
    for (int level = 0; level <= 3; level++) {
        CGLPixelFormatAttribute out[132];
        int n = 0;
        bool have_prof = false;
        for (int i = 0; i < nin;) {
            int a = in[i];
            bool val = pfa_has_value(a);
            int drop = pfa_drop_level(a);
            if (!drop || level < drop) {
                out[n++] = (CGLPixelFormatAttribute)a;
                if (val) {
                    int v = in[i + 1];
                    if (a == kCGLPFAOpenGLProfile) {
                        have_prof = true;
                        if (prof) v = prof;
                    }
                    out[n++] = (CGLPixelFormatAttribute)v;
                }
            }
            i += val ? 2 : 1;
        }
        if (!have_prof && prof) {
            out[n++] = kCGLPFAOpenGLProfile;
            out[n++] = (CGLPixelFormatAttribute)prof;
        }
        out[n] = 0;
        pix = NULL;
        npix = 0;
        err = CGLChoosePixelFormat(out, &pix, &npix);
        if (err == kCGLNoError && pix) {
            if (level) LOGI("cgl: pixel format found after dropping attributes (level %d)", level);
            break;
        }
        LOGI("cgl: CGLChoosePixelFormat level %d failed: %s", level, CGLErrorString(err));
    }
    if (err == kCGLNoError && !pix) err = kCGLBadPixelFormat;
    if (ARG(1)) gwrite32(ARG(1), hobj_wrap(pix));
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)npix);
    RET(err);
}

THUNK(CGLDestroyPixelFormat) { RET(CGLDestroyPixelFormat(PIX(0))); }
THUNK(CGLReleasePixelFormat) { CGLReleasePixelFormat(PIX(0)); }
THUNK(CGLRetainPixelFormat) { RET(hobj_wrap(CGLRetainPixelFormat(PIX(0)))); }
THUNK(CGLGetPixelFormatRetainCount) { RET(CGLGetPixelFormatRetainCount(PIX(0))); }
THUNK(CGLDescribePixelFormat) { RET(CGLDescribePixelFormat(PIX(0), ARGI(1), ARG(2), ARGP(3))); }
THUNK(CGLGetPixelFormat) { RET(hobj_wrap(CGLGetPixelFormat(CTX(0)))); }

// --- renderer info ------------------------------------------------------------------
THUNK(CGLQueryRendererInfo) {
    CGLRendererInfoObj ri = NULL;
    GLint n = 0;
    CGLError err = CGLQueryRendererInfo(ARG(0), &ri, &n);
    LOGI("cgl: CGLQueryRendererInfo(mask %#x) -> %d renderers, err %d", ARG(0), n, err);
    if (ARG(1)) gwrite32(ARG(1), hobj_wrap(ri));
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)n);
    RET(err);
}
THUNK(CGLDestroyRendererInfo) {
    CGLRendererInfoObj ri = hobj_unwrap(ARG(0));
    CGLError err = CGLDestroyRendererInfo(ri);
    if (err == kCGLNoError) hobj_forget(ARG(0));
    RET(err);
}
THUNK(CGLDescribeRenderer) {
    GLint v = 0;
    CGLError err = CGLDescribeRenderer(hobj_unwrap(ARG(0)), ARGI(1), ARG(2), &v);
    // Apple Silicon reports its unified memory as VRAM (tens of GB); 32-bit
    // games compute byte counts from it and overflow. Report the same capped
    // size as the IOKit VRAM properties (DXH_VRAM_MB, default 2048).
    if (err == kCGLNoError) {
        const char *env = getenv("DXH_VRAM_MB");
        GLint cap_mb = env ? atoi(env) : 2048;
        if (cap_mb <= 0 || cap_mb > 4095) cap_mb = 2048;
        switch (ARG(2)) {
        case kCGLRPVideoMemoryMegabytes:
        case kCGLRPTextureMemoryMegabytes:
            if (v > cap_mb) v = cap_mb;
            break;
        case kCGLRPVideoMemory:
        case kCGLRPTextureMemory:
            if ((uint32_t)v > (uint32_t)cap_mb << 20 || v < 0) v = (GLint)((uint32_t)cap_mb << 20);
            break;
        default: break;
        }
    }
    LOGD("cgl: CGLDescribeRenderer(%d, prop %d) = %d (err %d)", ARGI(1), ARG(2), v, err);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)v);
    RET(err);
}

// --- contexts -------------------------------------------------------------------------
THUNK(CGLCreateContext) {
    CGLContextObj ctx = NULL;
    CGLError err = CGLCreateContext(PIX(0), CTX(1), &ctx);
    LOGI("cgl: CGLCreateContext -> %p (err %d)", (void *)ctx, err);
    if (ARG(2)) gwrite32(ARG(2), hobj_wrap(ctx));
    RET(err);
}
THUNK(CGLDestroyContext) {
    CGLContextObj ctx = CTX(0);
    gl_ctx_forget(ctx);
    CGLError err = CGLDestroyContext(ctx);
    if (err == kCGLNoError) hobj_forget(ARG(0));
    RET(err);
}
THUNK(CGLRetainContext) { RET(hobj_wrap(CGLRetainContext(CTX(0)))); }
THUNK(CGLReleaseContext) {
    CGLContextObj ctx = CTX(0);
    if (ctx && CGLGetContextRetainCount(ctx) == 1) gl_ctx_forget(ctx);
    CGLReleaseContext(ctx);
}
THUNK(CGLGetContextRetainCount) { RET(CGLGetContextRetainCount(CTX(0))); }
THUNK(CGLCopyContext) { RET(CGLCopyContext(CTX(0), CTX(1), ARG(2))); }
THUNK(CGLSetCurrentContext) { RET(CGLSetCurrentContext(CTX(0))); }
THUNK(CGLGetCurrentContext) { RET(hobj_wrap(CGLGetCurrentContext())); }
THUNK(CGLGetShareGroup) { RET(hobj_wrap(CGLGetShareGroup(CTX(0)))); }
// Context locks held by the current thread (CGL locks are recursive). A thread
// that hops to the main thread (objc_msg.m) drops them meanwhile: AppKit's GL
// view layer takes the same lock on the main thread (e.g. entering fullscreen).
#define CGL_HELD_MAX 8
static __thread struct { CGLContextObj ctx; int depth; } cgl_held[CGL_HELD_MAX];

THUNK(CGLLockContext) {
    CGLContextObj c = CTX(0);
    CGLError e = CGLLockContext(c);
    if (e == kCGLNoError) {
        int free_slot = -1;
        for (int i = 0; i < CGL_HELD_MAX; i++) {
            if (cgl_held[i].ctx == c) {
                cgl_held[i].depth++;
                RET(e);
            }
            if (!cgl_held[i].ctx && free_slot < 0) free_slot = i;
        }
        if (free_slot >= 0) cgl_held[free_slot].ctx = c, cgl_held[free_slot].depth = 1;
    }
    RET(e);
}
THUNK(CGLUnlockContext) {
    CGLContextObj c = CTX(0);
    for (int i = 0; i < CGL_HELD_MAX; i++)
        if (cgl_held[i].ctx == c && --cgl_held[i].depth == 0) cgl_held[i].ctx = NULL;
    RET(CGLUnlockContext(c));
}

void cgl_drop_held_locks(void) {
    for (int i = 0; i < CGL_HELD_MAX; i++)
        for (int d = 0; cgl_held[i].ctx && d < cgl_held[i].depth; d++) CGLUnlockContext(cgl_held[i].ctx);
}
void cgl_retake_held_locks(void) {
    for (int i = 0; i < CGL_HELD_MAX; i++)
        for (int d = 0; cgl_held[i].ctx && d < cgl_held[i].depth; d++) CGLLockContext(cgl_held[i].ctx);
}
THUNK(CGLUpdateContext) { RET(CGLUpdateContext(CTX(0))); }

// DXH_FPS=<seconds>: frame statistics (frames/s, worst frame, frames over 33 ms,
// time spent inside the flush) from the presented frames.
static void fps_frame(uint64_t flush_ns) {
    static int interval = -1;
    static uint64_t last, window_start, worst, in_flush;
    static unsigned frames, slow;
    if (interval < 0) {
        const char *e = getenv("DXH_FPS");
        interval = e ? atoi(e) : 0;
    }
    if (!interval) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (last) {
        uint64_t dt = now - last;
        if (dt > worst) worst = dt;
        if (dt > 33000000) slow++;
    } else {
        window_start = now;
    }
    last = now;
    frames++;
    in_flush += flush_ns;
    if (now - window_start >= (uint64_t)interval * 1000000000ull) {
        double secs = (now - window_start) / 1e9;
        extern volatile int cb_app_active; // carbon_event.m: Feral throttles to ~9 fps in the background
        extern uint64_t gl_shadow_bytes; // gl_special.c: persistent buffer shadows
        LOGI("fps: %.1f frames/s, worst %.1f ms, %u frames > 33 ms, %.1f ms/frame in flush, guest mem %u MB (shadows "
             "%u MB)%s",
             frames / secs, worst / 1e6, slow, frames ? in_flush / 1e6 / frames : 0,
             gmem_used_pages() >> (20 - GPAGE_SHIFT), (unsigned)(gl_shadow_bytes >> 20),
             cb_app_active ? "" : " (in background)");
        window_start = now;
        frames = slow = 0;
        worst = in_flush = 0;
    }
}

// --- frame capture (autoplay.m) -------------------------------------------------------------
// The bot asks for the next presented frame; the flush reads the back buffer
// before presenting it. Pixels are RGBA, bottom row first.

#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif

struct glshot {
    int w, h;
    uint8_t *px;
    dispatch_semaphore_t done;
};
static struct glshot *_Atomic shot_req;
_Atomic uint64_t gl_frames; // presented frames

static void shot_capture(CGLContextObj ctx, struct glshot *r) {
    GLint wh[2] = {0, 0}, vp[4];
    CGLGetParameter(ctx, kCGLCPSurfaceBackingSize, wh);
    if (wh[0] <= 0 || wh[1] <= 0) {
        glGetIntegerv(GL_VIEWPORT, vp);
        wh[0] = vp[2], wh[1] = vp[3];
    }
    if (wh[0] <= 0 || wh[1] <= 0) return;
    GLint rfb, pbo, rbuf, align, rowlen, skiprows, skippix;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowlen);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skiprows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skippix);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &rbuf);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadBuffer(GL_BACK);
    r->px = malloc((size_t)wh[0] * wh[1] * 4);
    glReadPixels(0, 0, wh[0], wh[1], GL_RGBA, GL_UNSIGNED_BYTE, r->px);
    r->w = wh[0], r->h = wh[1];
    glReadBuffer(rbuf);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowlen);
    glPixelStorei(GL_PACK_SKIP_ROWS, skiprows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skippix);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
}

// Bot thread: the next presented frame, or NULL after timeout seconds.
uint8_t *gl_capture_frame(int *w, int *h, double timeout) {
    struct glshot *r = calloc(1, sizeof *r);
    r->done = dispatch_semaphore_create(0);
    struct glshot *none = NULL;
    if (!atomic_compare_exchange_strong(&shot_req, &none, r)) {
        free(r);
        return NULL;
    }
    if (dispatch_semaphore_wait(r->done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * 1e9)))) {
        struct glshot *mine = r;
        if (atomic_compare_exchange_strong(&shot_req, &mine, NULL)) { // not taken: no frame
            free(r);
            return NULL;
        }
        dispatch_semaphore_wait(r->done, DISPATCH_TIME_FOREVER); // being captured right now
    }
    uint8_t *px = r->px;
    *w = r->w, *h = r->h;
    free(r);
    return px;
}

THUNK(CGLFlushDrawable) {
    struct glshot *req = atomic_exchange(&shot_req, NULL);
    if (req) {
        shot_capture(CTX(0), req);
        dispatch_semaphore_signal(req->done);
    }
    atomic_fetch_add(&gl_frames, 1);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    CGLError e = CGLFlushDrawable(CTX(0));
    fps_frame(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    if (spikes_on) spikes_frame();
    RET(e);
}
THUNK(CGLClearDrawable) { RET(CGLClearDrawable(CTX(0))); }

THUNK(CGLEnable) {
    LOGI("cgl: CGLEnable(%d)", ARG(1));
    RET(CGLEnable(CTX(0), ARG(1)));
}
THUNK(CGLDisable) {
    LOGI("cgl: CGLDisable(%d)", ARG(1));
    RET(CGLDisable(CTX(0), ARG(1)));
}
THUNK(CGLIsEnabled) { RET(CGLIsEnabled(CTX(0), ARG(1), ARGP(2))); }

// Number of GLints a context parameter carries.
static int cp_count(int pname) {
    switch (pname) {
    case kCGLCPSwapRectangle: return 4;
    case kCGLCPSurfaceBackingSize: return 2;
    case kCGLCPSurfaceTexture: return 3;
    case kCGLCPReclaimResources: return 0;
    default: return 1;
    }
}

THUNK(CGLSetParameter) {
    int pname = ARGI(1);
    const GLint *p = ARGP(2);
    if (pname == kCGLCPClientStorage) {
        // Pointer-sized on the host: keep the guest's 32-bit value zero-extended.
        GLint v[2] = {p ? p[0] : 0, 0};
        RET(CGLSetParameter(CTX(0), pname, v));
        return;
    }
    LOGI("cgl: CGLSetParameter(%d, %d%s)", pname, p && cp_count(pname) ? p[0] : 0, cp_count(pname) > 1 ? ", ..." : "");
    if (pname == kCGLCPSurfaceBackingSize && p) {
        GLint wh[2] = {p[0], p[1]};
        gl_scale_backing(wh); // DXH_RENDER_SCALE
        RET(CGLSetParameter(CTX(0), pname, wh));
        return;
    }
    RET(CGLSetParameter(CTX(0), pname, p));
}
THUNK(CGLGetParameter) {
    int pname = ARGI(1);
    GLint v[8] = {0};
    CGLError err = CGLGetParameter(CTX(0), pname, v);
    if (pname == kCGLCPSurfaceBackingSize) gl_scale_backing_back(v);
    int n = pname == kCGLCPClientStorage ? 1 : cp_count(pname);
    if (ARG(2))
        for (int i = 0; i < n; i++) gwrite32(ARG(2) + 4 * (uint32_t)i, (uint32_t)v[i]);
    RET(err);
}

THUNK(CGLSetVirtualScreen) { RET(CGLSetVirtualScreen(CTX(0), ARGI(1))); }
THUNK(CGLGetVirtualScreen) { RET(CGLGetVirtualScreen(CTX(0), ARGP(1))); }
THUNK(CGLSetGlobalOption) { RET(CGLSetGlobalOption(ARG(0), ARGP(1))); }
THUNK(CGLGetGlobalOption) { RET(CGLGetGlobalOption(ARG(0), ARGP(1))); }
THUNK(CGLSetOption) { RET(CGLSetOption(ARG(0), ARGI(1))); }
THUNK(CGLGetOption) { RET(CGLGetOption(ARG(0), ARGP(1))); }
THUNK(CGLGetVersion) { CGLGetVersion(ARGP(0), ARGP(1)); }

THUNK(CGLErrorString) {
    static gaddr_t cache[32];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    CGLError e = ARG(0);
    unsigned i = (unsigned)e - 10000u;
    const char *s = CGLErrorString(e);
    if (i >= 32) {
        RET(guest_static_string(0, s ? s : "unknown error"));
        return;
    }
    pthread_mutex_lock(&lock);
    if (!cache[i]) cache[i] = gheap_strdup(s ? s : "unknown error");
    gaddr_t g = cache[i];
    pthread_mutex_unlock(&lock);
    RET(g);
}

// Fullscreen / offscreen / surface attachment: obsolete or handled by the
// windowing layer. Log and fail cleanly.
THUNK(CGLSetFullScreen) {
    LOGE("cgl: CGLSetFullScreen not supported");
    RET(kCGLBadDrawable);
}
THUNK(CGLSetFullScreenOnDisplay) {
    LOGE("cgl: CGLSetFullScreenOnDisplay(%#x) not supported", ARG(1));
    RET(kCGLBadDrawable);
}
THUNK(CGLSetOffScreen) {
    LOGE("cgl: CGLSetOffScreen(%dx%d) not supported", ARGI(1), ARGI(2));
    RET(kCGLBadDrawable);
}
THUNK(CGLSetSurface) {
    LOGE("cgl: CGLSetSurface(cid %#x wid %#x sid %#x) not supported", ARG(1), ARG(2), ARG(3));
    RET(kCGLBadDrawable);
}
