// CoreGraphics display services: displays, display modes, gamma, cursor,
// window levels, reconfiguration callbacks.
//
// Display IDs, mode handles and flags pass through to the host. Sizes reported
// by CGDisplayBounds / CGDisplayModeGetWidth/Height are in *points* (on a Retina
// display running "looks like 1920x1080" the backing store is 3840x2160).
//
// Environment:
//   DXH_DISPLAY_PIXELS=1    CGDisplayModeGetWidth/Height and CGDisplayPixelsWide/High
//                           report backing pixels instead of points.
//   DXH_DISPLAY_MODES=WxH[@Hz],...  replace the mode list with synthetic modes.
//   DXH_ALLOW_GAMMA=1       really change display gamma (default: pretend).
//   DXH_ALLOW_CAPTURE=1     really capture displays / switch modes (default: pretend).
//   DXH_NO_MOUSE_GRAB=1     ignore cursor warping and mouse/cursor dissociation.
#include "cg_internal.h"

#include <pthread.h>
#include <stdlib.h>

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

// --- synthetic display modes ---------------------------------------------------------
// Represented as CFDictionaries {"Width","Height","RefreshRate"}; every
// CGDisplayMode* thunk accepts them.

static CFArrayRef fake_modes;
static pthread_once_t fake_once = PTHREAD_ONCE_INIT;

static CFDictionaryRef make_fake_mode(long w, long h, double hz) {
    CFStringRef keys[3] = {CFSTR("Width"), CFSTR("Height"), CFSTR("RefreshRate")};
    CFNumberRef vals[3] = {CFNumberCreate(NULL, kCFNumberLongType, &w), CFNumberCreate(NULL, kCFNumberLongType, &h),
                           CFNumberCreate(NULL, kCFNumberDoubleType, &hz)};
    CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 3,
                                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 0; i < 3; i++) CFRelease(vals[i]);
    return d;
}

static void init_fake_modes(void) {
    const char *spec = getenv("DXH_DISPLAY_MODES");
    if (!spec || !*spec) return;
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    const char *p = spec;
    while (*p) {
        char *end;
        long w = strtol(p, &end, 10);
        long h = 0;
        double hz = 60;
        if (*end == 'x') h = strtol(end + 1, &end, 10);
        if (*end == '@') hz = strtod(end + 1, &end);
        if (w > 0 && h > 0) {
            CFDictionaryRef m = make_fake_mode(w, h, hz);
            CFArrayAppendValue(a, m);
            CFRelease(m);
            LOGI("cg: synthetic display mode %ldx%ld@%g", w, h, hz);
        }
        while (*end && *end != ',') end++;
        p = *end ? end + 1 : end;
    }
    if (CFArrayGetCount(a)) fake_modes = a;
    else CFRelease(a);
}

static CFArrayRef get_fake_modes(void) {
    pthread_once(&fake_once, init_fake_modes);
    return fake_modes;
}

static bool is_fake_mode(CFTypeRef m) { return m && CFGetTypeID(m) == CFDictionaryGetTypeID(); }

static double fake_num(CFTypeRef m, CFStringRef key) {
    double v = 0;
    CFNumberRef n = CFDictionaryGetValue((CFDictionaryRef)m, key);
    if (n) CFNumberGetValue(n, kCFNumberDoubleType, &v);
    return v;
}

static bool report_pixels(void) {
    static int v = -1;
    if (v < 0) v = env_flag("DXH_DISPLAY_PIXELS");
    return v;
}

// --- displays ------------------------------------------------------------------------------

THUNK(CGMainDisplayID) { RET(CGMainDisplayID()); }

THUNK(CGGetActiveDisplayList) {
    uint32_t max = ARG(0), n = 0;
    CGDirectDisplayID ids[32];
    CGError e = CGGetActiveDisplayList(max < 32 ? max : 32, ARG(1) ? ids : NULL, &n);
    if (ARG(1))
        for (uint32_t i = 0; i < n && i < max; i++) gwrite32(ARG(1) + 4 * i, ids[i]);
    if (ARG(2)) gwrite32(ARG(2), n);
    RET(e);
}

THUNK(CGGetOnlineDisplayList) {
    uint32_t max = ARG(0), n = 0;
    CGDirectDisplayID ids[32];
    CGError e = CGGetOnlineDisplayList(max < 32 ? max : 32, ARG(1) ? ids : NULL, &n);
    if (ARG(1))
        for (uint32_t i = 0; i < n && i < max; i++) gwrite32(ARG(1) + 4 * i, ids[i]);
    if (ARG(2)) gwrite32(ARG(2), n);
    RET(e);
}

// CGRect CGDisplayBounds(CGDirectDisplayID) -- hidden struct-return pointer at ARG(0).
THUNK(CGDisplayBounds) {
    CGRect b = CGDisplayBounds(ARG(1));
    LOGD("cg: CGDisplayBounds(%u) = %g,%g %gx%g", ARG(1), b.origin.x, b.origin.y, b.size.width, b.size.height);
    RET_RECT(b);
}

static size_t display_pixels(CGDirectDisplayID d, bool wide) {
    if (report_pixels()) {
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        if (m) {
            size_t v = wide ? CGDisplayModeGetPixelWidth(m) : CGDisplayModeGetPixelHeight(m);
            CGDisplayModeRelease(m);
            return v;
        }
    }
    return wide ? CGDisplayPixelsWide(d) : CGDisplayPixelsHigh(d);
}
THUNK(CGDisplayPixelsWide) { RET(display_pixels(ARG(0), true)); }
THUNK(CGDisplayPixelsHigh) { RET(display_pixels(ARG(0), false)); }

// CGSize CGDisplayScreenSize(display): 8-byte struct in edx:eax (millimetres).
THUNK(CGDisplayScreenSize) {
    CGSize s = CGDisplayScreenSize(ARG(0));
    RET_SIZE(s);
}

THUNK(CGDisplayIDToOpenGLDisplayMask) { RET(CGDisplayIDToOpenGLDisplayMask(ARG(0))); }
THUNK(CGOpenGLDisplayMaskToDisplayID) { RET(CGOpenGLDisplayMaskToDisplayID(ARG(0))); }
THUNK(CGDisplayIsMain) { RET(CGDisplayIsMain(ARG(0))); }
THUNK(CGDisplayIsActive) { RET(CGDisplayIsActive(ARG(0))); }
THUNK(CGDisplayIsBuiltin) { RET(CGDisplayIsBuiltin(ARG(0))); }
THUNK(CGDisplayIsOnline) { RET(CGDisplayIsOnline(ARG(0))); }
THUNK(CGDisplayModelNumber) { RET(CGDisplayModelNumber(ARG(0))); }
THUNK(CGDisplayVendorNumber) { RET(CGDisplayVendorNumber(ARG(0))); }
THUNK(CGDisplayUnitNumber) { RET(CGDisplayUnitNumber(ARG(0))); }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// io_service_t is a Mach port name of this (host) task; the IOKit thunks pass
// port names through unchanged, so the host value is usable by the guest.
THUNK(CGDisplayIOServicePort) { RET(CGDisplayIOServicePort(ARG(0))); }
#pragma clang diagnostic pop

// --- display modes -------------------------------------------------------------------------------

// CFArrayRef CGDisplayCopyAllDisplayModes(display, CFDictionaryRef options)
THUNK(CGDisplayCopyAllDisplayModes) {
    CFArrayRef fake = get_fake_modes();
    if (fake) {
        CFRetain(fake);
        RET_CF(fake);
        return;
    }
    CFArrayRef a = CGDisplayCopyAllDisplayModes(ARG(0), (CFDictionaryRef)ARGCF(1));
    LOGI("cg: CGDisplayCopyAllDisplayModes(%u) -> %ld modes", ARG(0), a ? (long)CFArrayGetCount(a) : -1L);
    RET_CF(a);
}

THUNK(CGDisplayCopyDisplayMode) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(ARG(0));
    CFArrayRef fake = get_fake_modes();
    if (fake && m) {
        // Report the synthetic mode matching the real pixel size, else the first.
        size_t pw = CGDisplayModeGetPixelWidth(m), ph = CGDisplayModeGetPixelHeight(m);
        size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m);
        CFTypeRef pick = CFArrayGetValueAtIndex(fake, 0);
        for (CFIndex i = 0; i < CFArrayGetCount(fake); i++) {
            CFTypeRef f = CFArrayGetValueAtIndex(fake, i);
            size_t fw = (size_t)fake_num(f, CFSTR("Width")), fh = (size_t)fake_num(f, CFSTR("Height"));
            if ((fw == pw && fh == ph) || (fw == w && fh == h)) {
                pick = f;
                break;
            }
        }
        CGDisplayModeRelease(m);
        CFRetain(pick);
        RET_CF(pick);
        return;
    }
    RET_CF(m);
}

THUNK(CGDisplayModeGetWidth) {
    CFTypeRef m = ARGCF(0);
    if (!m) { RET(0); return; }
    else if (is_fake_mode(m)) { RET((uint32_t)fake_num(m, CFSTR("Width"))); return; }
    else { RET(report_pixels() ? CGDisplayModeGetPixelWidth((CGDisplayModeRef)m) : CGDisplayModeGetWidth((CGDisplayModeRef)m)); return; }
}
THUNK(CGDisplayModeGetHeight) {
    CFTypeRef m = ARGCF(0);
    if (!m) { RET(0); return; }
    else if (is_fake_mode(m)) { RET((uint32_t)fake_num(m, CFSTR("Height"))); return; }
    else { RET(report_pixels() ? CGDisplayModeGetPixelHeight((CGDisplayModeRef)m) : CGDisplayModeGetHeight((CGDisplayModeRef)m)); return; }
}
THUNK(CGDisplayModeGetPixelWidth) {
    CFTypeRef m = ARGCF(0);
    if (!m) { RET(0); return; }
    else if (is_fake_mode(m)) { RET((uint32_t)fake_num(m, CFSTR("Width"))); return; }
    else { RET(CGDisplayModeGetPixelWidth((CGDisplayModeRef)m)); return; }
}
THUNK(CGDisplayModeGetPixelHeight) {
    CFTypeRef m = ARGCF(0);
    if (!m) { RET(0); return; }
    else if (is_fake_mode(m)) { RET((uint32_t)fake_num(m, CFSTR("Height"))); return; }
    else { RET(CGDisplayModeGetPixelHeight((CGDisplayModeRef)m)); return; }
}
THUNK(CGDisplayModeGetRefreshRate) {
    CFTypeRef m = ARGCF(0);
    double v = 0;
    if (is_fake_mode(m)) v = fake_num(m, CFSTR("RefreshRate"));
    else if (m) v = CGDisplayModeGetRefreshRate((CGDisplayModeRef)m);
    RETD(v);
}
THUNK(CGDisplayModeGetIOFlags) {
    CFTypeRef m = ARGCF(0);
    if (is_fake_mode(m)) { RET(kDisplayModeValidFlag | kDisplayModeSafeFlag); return; }
    else { RET(m ? CGDisplayModeGetIOFlags((CGDisplayModeRef)m) : 0); return; }
}
THUNK(CGDisplayModeGetIODisplayModeID) {
    CFTypeRef m = ARGCF(0);
    RET(m && !is_fake_mode(m) ? CGDisplayModeGetIODisplayModeID((CGDisplayModeRef)m) : 0);
}
THUNK(CGDisplayModeIsUsableForDesktopGUI) {
    CFTypeRef m = ARGCF(0);
    RET(is_fake_mode(m) ? 1 : m ? CGDisplayModeIsUsableForDesktopGUI((CGDisplayModeRef)m) : 0);
}
THUNK(CGDisplayModeGetTypeID) { RET(CGDisplayModeGetTypeID()); }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
THUNK(CGDisplayModeCopyPixelEncoding) {
    CFTypeRef m = ARGCF(0);
    CFStringRef e = NULL;
    if (m && !is_fake_mode(m)) e = CGDisplayModeCopyPixelEncoding((CGDisplayModeRef)m);
    // The game probably filters modes by 32-bit pixel encoding; modern macOS
    // may report 30-bit or nothing at all, which an old game would reject.
    if (!e || CFStringCompare(e, CFSTR(IO32BitDirectPixels), 0) != kCFCompareEqualTo) {
        if (e) {
            char buf[64] = "";
            CFStringGetCString(e, buf, sizeof buf, kCFStringEncodingUTF8);
            LOGD("cg: pixel encoding %s reported as 32-bit", buf);
            CFRelease(e);
        }
        e = CFStringCreateCopy(NULL, CFSTR(IO32BitDirectPixels));
    }
    RET_CF(e);
}
#pragma clang diagnostic pop

THUNK(CGDisplayModeRetain) {
    CFTypeRef m = ARGCF(0);
    if (m) CFRetain(m);
    RET(ARG(0));
}
THUNK(CGDisplayModeRelease) {
    CFTypeRef m = ARGCF(0);
    if (m) CFRelease(m);
}

// --- capture / mode switching (pretend unless DXH_ALLOW_CAPTURE) ------------------------------------

THUNK(CGDisplayCapture) {
    LOGI("cg: CGDisplayCapture(%u)%s", ARG(0), env_flag("DXH_ALLOW_CAPTURE") ? "" : " (pretend)");
    RET(env_flag("DXH_ALLOW_CAPTURE") ? CGDisplayCapture(ARG(0)) : kCGErrorSuccess);
}
THUNK(CGDisplayCaptureWithOptions) {
    LOGI("cg: CGDisplayCaptureWithOptions(%u)%s", ARG(0), env_flag("DXH_ALLOW_CAPTURE") ? "" : " (pretend)");
    RET(env_flag("DXH_ALLOW_CAPTURE") ? CGDisplayCaptureWithOptions(ARG(0), ARG(1)) : kCGErrorSuccess);
}
THUNK(CGCaptureAllDisplays) {
    LOGI("cg: CGCaptureAllDisplays%s", env_flag("DXH_ALLOW_CAPTURE") ? "" : " (pretend)");
    RET(env_flag("DXH_ALLOW_CAPTURE") ? CGCaptureAllDisplays() : kCGErrorSuccess);
}
THUNK(CGDisplayRelease) {
    LOGI("cg: CGDisplayRelease(%u)", ARG(0));
    RET(env_flag("DXH_ALLOW_CAPTURE") ? CGDisplayRelease(ARG(0)) : kCGErrorSuccess);
}
THUNK(CGReleaseAllDisplays) {
    LOGI("cg: CGReleaseAllDisplays");
    RET(env_flag("DXH_ALLOW_CAPTURE") ? CGReleaseAllDisplays() : kCGErrorSuccess);
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
THUNK(CGDisplayIsCaptured) { RET(env_flag("DXH_ALLOW_CAPTURE") ? CGDisplayIsCaptured(ARG(0)) : 0); }
#pragma clang diagnostic pop
THUNK(CGDisplaySetDisplayMode) {
    CFTypeRef m = ARGCF(1);
    if (is_fake_mode(m))
        LOGI("cg: CGDisplaySetDisplayMode(%u, synthetic %gx%g) (pretend)", ARG(0), fake_num(m, CFSTR("Width")),
             fake_num(m, CFSTR("Height")));
    else if (m)
        LOGI("cg: CGDisplaySetDisplayMode(%u, %zux%zu)%s", ARG(0), CGDisplayModeGetWidth((CGDisplayModeRef)m),
             CGDisplayModeGetHeight((CGDisplayModeRef)m), env_flag("DXH_ALLOW_CAPTURE") ? "" : " (pretend)");
    if (env_flag("DXH_ALLOW_CAPTURE") && m && !is_fake_mode(m))
        RET(CGDisplaySetDisplayMode(ARG(0), (CGDisplayModeRef)m, (CFDictionaryRef)ARGCF(2)));
    else
        RET(kCGErrorSuccess);
}
THUNK(CGShieldingWindowLevel) { RET((uint32_t)CGShieldingWindowLevel()); }
THUNK(CGShieldingWindowID) { RET(CGShieldingWindowID(ARG(0))); }
THUNK(CGWindowLevelForKey) { RET((uint32_t)CGWindowLevelForKey((CGWindowLevelKey)ARGI(0))); }

// --- reconfiguration callbacks ---------------------------------------------------------------------
// void cb(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *userInfo)

struct reconf_cb {
    gaddr_t fn, info;
    struct reconf_cb *next;
};
static struct reconf_cb *reconf_list;
static pthread_mutex_t reconf_lock = PTHREAD_MUTEX_INITIALIZER;

static void reconf_trampoline(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *user) {
    struct reconf_cb *c = user;
    LOGD("cg: display reconfiguration %u flags %#x -> guest %#x", display, flags, c->fn);
    gcall3(c->fn, display, flags, c->info);
}

THUNK(CGDisplayRegisterReconfigurationCallback) {
    struct reconf_cb *c = calloc(1, sizeof *c);
    c->fn = ARG(0);
    c->info = ARG(1);
    pthread_mutex_lock(&reconf_lock);
    c->next = reconf_list;
    reconf_list = c;
    pthread_mutex_unlock(&reconf_lock);
    RET(CGDisplayRegisterReconfigurationCallback(reconf_trampoline, c));
}

THUNK(CGDisplayRemoveReconfigurationCallback) {
    pthread_mutex_lock(&reconf_lock);
    struct reconf_cb **pp = &reconf_list, *c = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->fn == ARG(0) && (*pp)->info == ARG(1)) {
            c = *pp;
            *pp = c->next;
            break;
        }
    pthread_mutex_unlock(&reconf_lock);
    if (!c) { RET(kCGErrorIllegalArgument); return; }
    else {
        RET(CGDisplayRemoveReconfigurationCallback(reconf_trampoline, c));
        // c is leaked on purpose: a callback may still be in flight.
    }
}

// --- gamma (CGGammaValue is float on both ABIs) ------------------------------------------------------

THUNK(CGGetDisplayTransferByFormula) {
    CGGammaValue v[9];
    CGError e = CGGetDisplayTransferByFormula(ARG(0), &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]);
    for (int i = 0; i < 9; i++)
        if (ARG(1 + i)) gwrite32(ARG(1 + i), f2u(v[i]));
    RET(e);
}

THUNK(CGGetDisplayTransferByTable) {
    // (display, capacity, red*, green*, blue*, uint32_t *sampleCount)
    uint32_t cap = ARG(1), n = 0;
    CGError e = CGGetDisplayTransferByTable(ARG(0), cap, ARGP(2), ARGP(3), ARGP(4), &n);
    if (ARG(5)) gwrite32(ARG(5), n);
    RET(e);
}

THUNK(CGSetDisplayTransferByTable) {
    if (!env_flag("DXH_ALLOW_GAMMA")) {
        LOGD("cg: CGSetDisplayTransferByTable(%u, %u samples) ignored (set DXH_ALLOW_GAMMA=1)", ARG(0), ARG(1));
        RET(kCGErrorSuccess);
        return;
    }
    RET(CGSetDisplayTransferByTable(ARG(0), ARG(1), ARGP(2), ARGP(3), ARGP(4)));
}

THUNK(CGSetDisplayTransferByFormula) {
    if (!env_flag("DXH_ALLOW_GAMMA")) {
        LOGD("cg: CGSetDisplayTransferByFormula(%u) ignored (set DXH_ALLOW_GAMMA=1)", ARG(0));
        RET(kCGErrorSuccess);
        return;
    }
    RET(CGSetDisplayTransferByFormula(ARG(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGF(6), ARGF(7), ARGF(8),
                                      ARGF(9)));
}

THUNK(CGDisplayGammaTableCapacity) { RET(CGDisplayGammaTableCapacity(ARG(0))); }

THUNK(CGDisplayRestoreColorSyncSettings) {
    if (env_flag("DXH_ALLOW_GAMMA")) CGDisplayRestoreColorSyncSettings();
    else LOGD("cg: CGDisplayRestoreColorSyncSettings (no-op, gamma untouched)");
}

// --- cursor / events ----------------------------------------------------------------------------------

// Window dragging: clicking the title bar of the (inactive) game window activates
// it, and the game grabs the mouse at once (dissociates and recentres the cursor),
// which kills the drag. A press that starts on the window frame (reported by
// carbon_event.m) defers grabbing until that button is released. Presses inside
// the game (aiming while shooting) are never affected.
static volatile bool frame_press;  // left button went down on a window frame
static volatile bool grab_pending; // the game asked for a grab during that press
static volatile bool grabbed;      // mouse dissociated by the game

// After a frame press the mouse stays free until the next click into the game
// (as windowed games usually behave), not just until the button is released.
static bool grab_blocked(void) { return frame_press; }

// carbon_event.m: a mouse-down inside a window's content.
void cg_mouse_content_press(void) {
    if (!frame_press) return;
    frame_press = false;
    if (grab_pending) {
        grab_pending = false;
        LOGD("cg: deferred mouse grab applied (click into the game)");
        CGAssociateMouseAndMouseCursorPosition(false);
        grabbed = true;
    }
}

// carbon_event.m: a mouse-down on the title bar/frame goes to AppKit.
void cg_mouse_frame_press(void) {
    frame_press = true;
    if (grabbed) { // the activation already grabbed it: let the drag work
        grabbed = false;
        grab_pending = true;
        CGAssociateMouseAndMouseCursorPosition(true);
    }
}

// The app lost focus: the user must get the mouse back, and the next activation
// (often a title bar click) starts ungrabbed.
void cg_mouse_app_deactivated(void) {
    grab_pending = false;
    frame_press = false;
    if (grabbed) {
        grabbed = false;
        CGAssociateMouseAndMouseCursorPosition(true);
    }
}

static void grab_resume(void) {} // deferred grabs are applied by cg_mouse_content_press

THUNK(CGWarpMouseCursorPosition) {
    CGPoint p = ARGPOINT(0);
    LOGT("cg: warp cursor to %g,%g", p.x, p.y);
    if (env_flag("DXH_NO_MOUSE_GRAB") || grab_blocked()) { RET(kCGErrorSuccess); return; }
    grab_resume();
    RET(CGWarpMouseCursorPosition(p));
}

THUNK(CGAssociateMouseAndMouseCursorPosition) {
    LOGD("cg: CGAssociateMouseAndMouseCursorPosition(%u)", ARG(0));
    if (env_flag("DXH_NO_MOUSE_GRAB")) { RET(kCGErrorSuccess); return; }
    if (!ARG(0) && grab_blocked()) {
        LOGD("cg: mouse grab deferred (window drag)");
        grab_pending = true;
        RET(kCGErrorSuccess);
        return;
    }
    grab_pending = false;
    grabbed = !ARG(0);
    RET(CGAssociateMouseAndMouseCursorPosition(ARG(0)));
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
THUNK(CGSetLocalEventsSuppressionInterval) { RET(CGSetLocalEventsSuppressionInterval(ARGD(0))); }
#pragma clang diagnostic pop

THUNK(CGDisplayHideCursor) { RET(CGDisplayHideCursor(ARG(0))); }
THUNK(CGDisplayShowCursor) { RET(CGDisplayShowCursor(ARG(0))); }
THUNK(CGDisplayMoveCursorToPoint) {
    if (env_flag("DXH_NO_MOUSE_GRAB") || grab_blocked()) { RET(kCGErrorSuccess); return; }
    else { RET(CGDisplayMoveCursorToPoint(ARG(0), ARGPOINT(1))); return; }
}
THUNK(CGGetLastMouseDelta) {
    grab_resume();
    int32_t dx = 0, dy = 0;
    CGGetLastMouseDelta(&dx, &dy);
    if (ARG(0)) gwrite32(ARG(0), (uint32_t)dx);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)dy);
}
