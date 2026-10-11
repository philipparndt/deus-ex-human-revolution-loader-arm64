// ApplicationServices odds and ends: Process Manager, Printing Manager,
// QuickDraw bridging stubs, icon plotting.
//
// The game runs inside the dxhost process, so "the current process" is the
// game: name, signature and bundle location are taken from the game's .app
// (cf_main_bundle()) instead of dxhost.
#include "cg_internal.h"

#include "../runtime.h"

#include <Carbon/Carbon.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// ProcessSerialNumber {UInt32 high, low} has the same layout on both ABIs.
static ProcessSerialNumber get_psn(gaddr_t g) {
    ProcessSerialNumber p = {0, 0};
    if (g) {
        p.highLongOfPSN = gread32(g);
        p.lowLongOfPSN = gread32(g + 4);
    }
    return p;
}
static void put_psn(gaddr_t g, ProcessSerialNumber p) {
    if (!g) return;
    gwrite32(g, p.highLongOfPSN);
    gwrite32(g + 4, p.lowLongOfPSN);
}

static bool is_self(ProcessSerialNumber p) {
    if (p.highLongOfPSN == 0 && p.lowLongOfPSN == kCurrentProcess) return true;
    ProcessSerialNumber me;
    if (GetCurrentProcess(&me) != noErr) return false;
    Boolean same = false;
    SameProcess(&p, &me, &same);
    return same;
}

static CFStringRef game_name(void) {
    CFBundleRef b = cf_main_bundle();
    CFStringRef n = b ? CFBundleGetValueForInfoDictionaryKey(b, kCFBundleNameKey) : NULL;
    if (n && CFGetTypeID(n) == CFStringGetTypeID()) return CFRetain(n);
    struct image *img = runtime_image(0);
    const char *path = img && img->path ? img->path : "game";
    const char *slash = strrchr(path, '/');
    return CFStringCreateWithCString(NULL, slash ? slash + 1 : path, kCFStringEncodingUTF8);
}

static uint32_t game_signature(void) {
    CFBundleRef b = cf_main_bundle();
    CFStringRef s = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CFBundleSignature")) : NULL;
    char buf[16] = "????";
    if (s && CFGetTypeID(s) == CFStringGetTypeID()) CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingMacRoman);
    return (uint32_t)(uint8_t)buf[0] << 24 | (uint32_t)(uint8_t)buf[1] << 16 | (uint32_t)(uint8_t)buf[2] << 8 |
           (uint8_t)buf[3];
}

THUNK(GetCurrentProcess) {
    ProcessSerialNumber p;
    OSErr e = GetCurrentProcess(&p);
    put_psn(ARG(0), p);
    RET((uint32_t)(int32_t)e);
}
THUNK(GetFrontProcess) {
    ProcessSerialNumber p;
    OSErr e = GetFrontProcess(&p);
    put_psn(ARG(0), p);
    RET((uint32_t)(int32_t)e);
}
THUNK(GetNextProcess) {
    ProcessSerialNumber p = get_psn(ARG(0));
    OSErr e = GetNextProcess(&p);
    put_psn(ARG(0), p);
    RET((uint32_t)(int32_t)e);
}
THUNK(SameProcess) {
    ProcessSerialNumber a = get_psn(ARG(0)), b = get_psn(ARG(1));
    Boolean same = false;
    OSErr e = SameProcess(&a, &b, &same);
    if (ARG(2)) gwrite8(ARG(2), same);
    RET((uint32_t)(int32_t)e);
}
THUNK(GetProcessPID) {
    ProcessSerialNumber p = get_psn(ARG(0));
    pid_t pid = 0;
    OSStatus e = GetProcessPID(&p, &pid);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)pid);
    RET(e);
}
THUNK(GetProcessForPID) {
    ProcessSerialNumber p = {0, 0};
    OSStatus e = GetProcessForPID(ARGI(0), &p);
    put_psn(ARG(1), p);
    RET(e);
}
THUNK(SetFrontProcess) {
    ProcessSerialNumber p = get_psn(ARG(0));
    RET((uint32_t)(int32_t)SetFrontProcess(&p));
}
THUNK(TransformProcessType) {
    ProcessSerialNumber p = get_psn(ARG(0));
    LOGI("as: TransformProcessType(%u)", ARG(1));
    RET(TransformProcessType(&p, ARG(1)));
}

// OSStatus CopyProcessName(const ProcessSerialNumber *, CFStringRef *name)
THUNK(CopyProcessName) {
    ProcessSerialNumber p = get_psn(ARG(0));
    CFStringRef name = NULL;
    OSStatus e = noErr;
    if (is_self(p)) name = game_name();
    else e = CopyProcessName(&p, &name);
    if (ARG(1)) gwrite32(ARG(1), cfg(name));
    RET(e);
}

// OSStatus GetProcessBundleLocation(const ProcessSerialNumber *, FSRef *) -- FSRef is 80 opaque bytes.
THUNK(GetProcessBundleLocation) {
    ProcessSerialNumber p = get_psn(ARG(0));
    if (is_self(p)) {
        CFURLRef url = CFBundleCopyBundleURL(cf_main_bundle());
        bool ok = url && CFURLGetFSRef(url, ARGP(1));
        if (url) CFRelease(url);
        RET(ok ? noErr : fnfErr);
        return;
    }
    RET(GetProcessBundleLocation(&p, ARGP(1)));
}

// ProcessInfoRec, i386 layout (#pragma pack(2), 60 bytes):
//   0 processInfoLength, 4 processName (StringPtr), 8 processNumber (PSN), 16 processType,
//   20 processSignature, 24 processMode, 28 processLocation (Ptr), 32 processSize,
//   36 processFreeMem, 40 processLauncher (PSN), 48 processLaunchDate, 52 processActiveTime,
//   56 processAppSpec (FSSpecPtr)
THUNK(GetProcessInformation) {
    ProcessSerialNumber p = get_psn(ARG(0));
    gaddr_t g = ARG(1);
    if (!g) { RET((uint32_t)paramErr); return; }
    else {
        ProcessInfoRec info;
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        Str255 name;
        FSRef ref;
        gaddr_t gname = gread32(g + 4), gspec = gread32(g + 56);
        info.processName = gname ? name : NULL;
        info.processAppRef = gspec ? &ref : NULL;
        OSErr e = GetProcessInformation(&p, &info);
        if (e != noErr) {
            RET((uint32_t)(int32_t)e);
            return;
        }
        bool self = is_self(p);
        if (self) {
            CFStringRef n = game_name();
            if (!CFStringGetPascalString(n, name, 32, kCFStringEncodingMacRoman)) name[0] = 0;
            CFRelease(n);
            info.processSignature = game_signature();
            info.processType = 'APPL';
        }
        if (gname) {
            uint8_t len = name[0] > 31 ? 31 : name[0]; // Str31
            gwrite8(gname, len);
            memcpy(g2h(gname + 1), name + 1, len);
        }
        put_psn(g + 8, info.processNumber);
        gwrite32(g + 16, info.processType);
        gwrite32(g + 20, info.processSignature);
        gwrite32(g + 24, info.processMode);
        gwrite32(g + 28, 0); // processLocation: no guest equivalent
        gwrite32(g + 32, info.processSize);
        gwrite32(g + 36, info.processFreeMem);
        put_psn(g + 40, info.processLauncher);
        gwrite32(g + 48, info.processLaunchDate);
        gwrite32(g + 52, info.processActiveTime);
        if (gspec) {
            // FSSpec {SInt16 vRefNum; SInt32 parID; Str63 name} -- no 64-bit equivalent;
            // fill in the leaf name only.
            memset(g2h(gspec), 0, 70);
            HFSUniStr255 leaf;
            if (FSGetCatalogInfo(&ref, kFSCatInfoNone, NULL, &leaf, NULL, NULL) == noErr) {
                CFStringRef s = CFStringCreateWithCharacters(NULL, leaf.unicode, leaf.length);
                Str255 pn;
                if (s && CFStringGetPascalString(s, pn, 64, kCFStringEncodingMacRoman))
                    memcpy(g2h(gspec + 6), pn, (size_t)pn[0] + 1);
                if (s) CFRelease(s);
            }
            LOGD("as: GetProcessInformation: processAppSpec (FSSpec) only carries the name");
        }
        RET(noErr);
    }
}

THUNK(ExitToShell) {
    LOGI("as: ExitToShell");
    runtime_exit(0);
}

// LaunchParamBlockRec uses FSSpec, which has no 64-bit equivalent.
THUNK(LaunchApplication) {
    LOGE("as: LaunchApplication(%#x) not supported", ARG(0));
    RET((uint32_t)procNotFound);
}

// --- QuickDraw bridging (QuickDraw ports do not exist on the 64-bit host) --------------------

THUNK(QDBeginCGContext) {
    LOGE("as: QDBeginCGContext(port %#x) not supported (no QuickDraw)", ARG(0));
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET((uint32_t)paramErr);
}
THUNK(QDEndCGContext) {
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET((uint32_t)paramErr);
}

// OSStatus PlotIconRefInContext(CGContextRef, const CGRect *, IconAlignmentType, IconTransformType,
//                               const RGBColor *labelColor, PlotIconRefFlags, IconRef)
// IconRef handles are expected to be hobj handles of host IconRefs (CoreServices thunks).
THUNK(PlotIconRefInContext) {
    CGRect rc = ARG(1) ? get_grect(ARG(1)) : CGRectZero;
    IconRef icon = hobj_unwrap(ARG(6));
    if (!icon) { RET((uint32_t)paramErr); return; }
    else
        RET(PlotIconRefInContext((CGContextRef)ARGCF(0), &rc, (IconAlignmentType)(int16_t)ARG(2),
                                 (IconTransformType)(int16_t)ARG(3), ARGP(4), ARG(5), icon));
}

// --- Printing Manager ---------------------------------------------------------------------------
// PM objects are opaque handles (hobj, not wrapped as CF so the ObjC bridge leaves them alone).

#define PMOBJ(i) hobj_unwrap(ARG(i))

THUNK(PMCreateSession) {
    PMPrintSession s = NULL;
    OSStatus e = PMCreateSession(&s);
    if (ARG(0)) gwrite32(ARG(0), hobj_wrap(s));
    RET(e);
}
THUNK(PMCreatePageFormat) {
    PMPageFormat f = NULL;
    OSStatus e = PMCreatePageFormat(&f);
    if (ARG(0)) gwrite32(ARG(0), hobj_wrap(f));
    RET(e);
}
THUNK(PMCreatePrintSettings) {
    PMPrintSettings s = NULL;
    OSStatus e = PMCreatePrintSettings(&s);
    if (ARG(0)) gwrite32(ARG(0), hobj_wrap(s));
    RET(e);
}
THUNK(PMSessionDefaultPageFormat) { RET(PMSessionDefaultPageFormat(PMOBJ(0), PMOBJ(1))); }
THUNK(PMSessionDefaultPrintSettings) { RET(PMSessionDefaultPrintSettings(PMOBJ(0), PMOBJ(1))); }
THUNK(PMSessionValidatePageFormat) {
    Boolean changed = false;
    OSStatus e = PMSessionValidatePageFormat(PMOBJ(0), PMOBJ(1), &changed);
    if (ARG(2)) gwrite8(ARG(2), changed);
    RET(e);
}
// PMRect {double top, left, bottom, right}: same offsets on i386.
THUNK(PMGetAdjustedPaperRect) {
    PMRect rc = {0};
    OSStatus e = PMGetAdjustedPaperRect(PMOBJ(0), &rc);
    if (ARG(1)) memcpy(ARGP(1), &rc, sizeof rc);
    RET(e);
}
THUNK(PMGetAdjustedPageRect) {
    PMRect rc = {0};
    OSStatus e = PMGetAdjustedPageRect(PMOBJ(0), &rc);
    if (ARG(1)) memcpy(ARGP(1), &rc, sizeof rc);
    RET(e);
}
// PMGetResolution was removed from the 64-bit API; report 72 dpi.
THUNK(PMGetResolution) {
    PMResolution res = {72.0, 72.0};
    if (ARG(1)) memcpy(ARGP(1), &res, sizeof res);
    RET(noErr);
}
THUNK(PMRelease) {
    void *o = PMOBJ(0);
    RET(o ? PMRelease(o) : (OSStatus)kPMInvalidParameter);
}
THUNK(PMRetain) {
    void *o = PMOBJ(0);
    RET(o ? PMRetain(o) : (OSStatus)kPMInvalidParameter);
}
