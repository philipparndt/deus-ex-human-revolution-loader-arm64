// CGL (OpenGL.framework context management).
//
// CGL objects (contexts, pixel formats, renderer infos, share groups) are host
// pointers and reach the guest as hobj handles. Attribute lists and GLint
// parameter arrays have the same layout on both sides (32-bit ints).
//
// Drawable attachment (CGLSetSurface, NSOpenGLContext views, AGL) is not done
// here; without a drawable the context renders into FBOs only.
#include "gl_internal.h"

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
THUNK(CGLLockContext) { RET(CGLLockContext(CTX(0))); }
THUNK(CGLUnlockContext) { RET(CGLUnlockContext(CTX(0))); }
THUNK(CGLUpdateContext) { RET(CGLUpdateContext(CTX(0))); }

THUNK(CGLFlushDrawable) { RET(CGLFlushDrawable(CTX(0))); }
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
    RET(CGLSetParameter(CTX(0), pname, p));
}
THUNK(CGLGetParameter) {
    int pname = ARGI(1);
    GLint v[8] = {0};
    CGLError err = CGLGetParameter(CTX(0), pname, v);
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
