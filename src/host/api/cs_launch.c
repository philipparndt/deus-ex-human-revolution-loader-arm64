// Launch Services and Icon Services.
//
// IconRef: hobj handle of the host IconRef (ApplicationServices' PlotIconRefInContext
// can hobj_unwrap() it).
#include "cf_bridge.h"
#include "cs_ae.h"

#include <CoreServices/CoreServices.h>

__asm__(".linker_option \"-framework\", \"CoreServices\"");

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define GRET_ERR(e) RET((uint32_t)(int32_t)(e))

// i386 FSCatalogInfo (144 bytes) -> host (148, see cs_files.c).
static void catinfo_from_guest(FSCatalogInfo *h, gaddr_t g) {
    uint8_t *hb = (uint8_t *)h;
    memset(h, 0, sizeof *h);
    memcpy(hb, g2h(g), 68);
    memcpy(hb + 76, g2h(g + 72), 72);
}

// --- Launch Services ---------------------------------------------------------------------
THUNK(LSCopyDisplayNameForURL) {
    CFStringRef name = NULL;
    OSStatus st = LSCopyDisplayNameForURL((CFURLRef)ARGCF(0), &name);
    if (ARG(1)) gwrite32(ARG(1), st == noErr ? cfg(name) : 0);
    GRET_ERR(st);
}

// LSItemInfoRecord (i386, 16 bytes): flags @0, filetype @4, creator @8, CFStringRef extension @12
THUNK(LSCopyItemInfoForRef) {
    LSItemInfoRecord info;
    memset(&info, 0, sizeof info);
    OSStatus st = LSCopyItemInfoForRef((const FSRef *)ARGP(0), ARG(1), &info);
    gaddr_t g = ARG(2);
    if (g) {
        gwrite32(g, info.flags);
        gwrite32(g + 4, info.filetype);
        gwrite32(g + 8, info.creator);
        gwrite32(g + 12, cfg(info.extension));
    }
    GRET_ERR(st);
}

THUNK(LSCopyItemInfoForURL) {
    LSItemInfoRecord info;
    memset(&info, 0, sizeof info);
    OSStatus st = LSCopyItemInfoForURL((CFURLRef)ARGCF(0), ARG(1), &info);
    gaddr_t g = ARG(2);
    if (g) {
        gwrite32(g, info.flags);
        gwrite32(g + 4, info.filetype);
        gwrite32(g + 8, info.creator);
        gwrite32(g + 12, cfg(info.extension));
    }
    GRET_ERR(st);
}

// LSFindApplicationForInfo(OSType creator, CFStringRef bundleID, CFStringRef name, FSRef *outAppRef, CFURLRef *outAppURL)
THUNK(LSFindApplicationForInfo) {
    CFURLRef url = NULL;
    OSStatus st = LSFindApplicationForInfo(ARG(0), ARGSTR(1), ARGSTR(2), (FSRef *)ARGP(3), ARG(4) ? &url : NULL);
    if (ARG(4)) gwrite32(ARG(4), st == noErr ? cfg(url) : 0);
    GRET_ERR(st);
}

THUNK(LSOpenCFURLRef) {
    CFURLRef url = (CFURLRef)ARGCF(0), launched = NULL;
    if (log_level >= 1 && url) {
        CFStringRef s = CFURLGetString(url);
        char *u = cf_utf8(s);
        LOGI("LSOpenCFURLRef(%s)", u);
        free(u);
    }
    OSStatus st = LSOpenCFURLRef(url, ARG(1) ? &launched : NULL);
    if (ARG(1)) gwrite32(ARG(1), st == noErr ? cfg(launched) : 0);
    GRET_ERR(st);
}

// LSLaunchFSRefSpec (i386, 24 bytes): appRef @0, numDocs @4, itemRefs @8 (FSRef array),
// passThruParams @12 (AEDesc*), launchFlags @16, asyncRefCon @20
THUNK(LSOpenFromRefSpec) {
    gaddr_t g = ARG(0);
    AEDesc pass;
    LSLaunchFSRefSpec spec;
    spec.appRef = (const FSRef *)g2h(gread32(g));
    spec.numDocs = gread32(g + 4);
    spec.itemRefs = (const FSRef *)g2h(gread32(g + 8));
    gaddr_t gpass = gread32(g + 12);
    aedesc_from_guest(&pass, gpass);
    spec.passThruParams = gpass ? &pass : NULL;
    spec.launchFlags = gread32(g + 16);
    spec.asyncRefCon = (void *)(uintptr_t)gread32(g + 20);
    LOGI("LSOpenFromRefSpec(%lu docs, flags %#x)", (unsigned long)spec.numDocs, (unsigned)spec.launchFlags);
    GRET_ERR(LSOpenFromRefSpec(&spec, (FSRef *)ARGP(1)));
}

// --- Icon Services ------------------------------------------------------------------------
static void put_iconref(gaddr_t g, OSStatus st, IconRef ic) {
    if (g) gwrite32(g, st == noErr ? hobj_wrap(ic) : 0);
}

THUNK(GetIconRef) {
    IconRef ic = NULL;
    OSErr e = GetIconRef((SInt16)ARG(0), ARG(1), ARG(2), &ic);
    put_iconref(ARG(3), e, ic);
    GRET_ERR(e);
}

// GetIconRefFromFileInfo(ref, nameLength, name, whichInfo, catInfo, usageFlags, IconRef*, SInt16 *outLabel)
THUNK(GetIconRefFromFileInfo) {
    IconRef ic = NULL;
    SInt16 label = 0;
    FSCatalogInfo ci;
    if (ARG(4)) catinfo_from_guest(&ci, ARG(4));
    OSStatus st = GetIconRefFromFileInfo((const FSRef *)ARGP(0), ARG(1), (const UniChar *)ARGP(2), ARG(3),
                                         ARG(4) ? &ci : NULL, ARG(5), &ic, &label);
    put_iconref(ARG(6), st, ic);
    if (ARG(7)) gwrite16(ARG(7), (uint16_t)label);
    GRET_ERR(st);
}

THUNK(RegisterIconRefFromFSRef) {
    IconRef ic = NULL;
    OSStatus st = RegisterIconRefFromFSRef(ARG(0), ARG(1), (const FSRef *)ARGP(2), &ic);
    put_iconref(ARG(3), st, ic);
    GRET_ERR(st);
}

THUNK(AcquireIconRef) { GRET_ERR(AcquireIconRef((IconRef)hobj_unwrap(ARG(0)))); }
THUNK(ReleaseIconRef) { GRET_ERR(ReleaseIconRef((IconRef)hobj_unwrap(ARG(0)))); }
