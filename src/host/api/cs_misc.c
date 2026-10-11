// CoreServices misc: Gestalt, timing (Microseconds/UpTime/Delay), debugging,
// user name, UTC conversions, UCKeyTranslate, Collection Manager.
#include "cf_bridge.h"

#include <CoreServices/CoreServices.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

__asm__(".linker_option \"-framework\", \"CoreServices\"");

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// --- Gestalt -------------------------------------------------------------------------
static int os_major = 10, os_minor = 9, os_bugfix = 5;
static bool os_version_ready;

static void init_os_version(void) {
    if (os_version_ready) return;
    os_version_ready = true;
    const char *v = getenv("DXH_FAKE_OSVERSION");
    if (v && strcmp(v, "host") == 0) {
        char buf[64] = {0};
        size_t len = sizeof buf - 1;
        if (sysctlbyname("kern.osproductversion", buf, &len, NULL, 0) == 0) v = buf;
        else v = NULL;
        if (v) {
            int a = 0, b = 0, c = 0;
            sscanf(v, "%d.%d.%d", &a, &b, &c);
            os_major = a, os_minor = b, os_bugfix = c;
        }
    } else if (v && *v) {
        int a = 10, b = 9, c = 0;
        if (sscanf(v, "%d.%d.%d", &a, &b, &c) >= 2) os_major = a, os_minor = b, os_bugfix = c;
        else LOGE("DXH_FAKE_OSVERSION=%s not understood (want e.g. 10.9.5)", v);
    }
    LOGI("Gestalt: reporting OS version %d.%d.%d", os_major, os_minor, os_bugfix);
}

static uint64_t sysctl_u64(const char *name, uint64_t dflt) {
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return dflt;
    if (len == 4) v = (uint32_t)v;
    return v;
}

static uint32_t cap32(uint64_t v) { return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)v; }

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

THUNK(Gestalt) {
    uint32_t sel = ARG(0);
    gaddr_t resp = ARG(1);
    init_os_version();
    uint32_t v;
    bool ok = true;
    switch (sel) {
    case FOURCC('s', 'y', 's', 'v'): { // BCD 0xMMmb, components capped at 9 like real 10.10+
        int mi = os_minor > 9 ? 9 : os_minor, bf = os_bugfix > 9 ? 9 : os_bugfix;
        int ma = os_major == 10 ? 0x10 : (os_major > 99 ? 0x99 : ((os_major / 10) << 4 | os_major % 10));
        v = (uint32_t)(ma << 8 | mi << 4 | bf);
        break;
    }
    case FOURCC('s', 'y', 's', '1'): v = (uint32_t)os_major; break;
    case FOURCC('s', 'y', 's', '2'): v = (uint32_t)os_minor; break;
    case FOURCC('s', 'y', 's', '3'): v = (uint32_t)os_bugfix; break;
    case FOURCC('r', 'a', 'm', 'm'): v = cap32(sysctl_u64("hw.memsize", 8ull << 30) >> 20); break;
    case FOURCC('r', 'a', 'm', ' '):
    case FOURCC('l', 'r', 'a', 'm'): v = cap32(sysctl_u64("hw.memsize", 8ull << 30)); break;
    case FOURCC('c', 'p', 'u', 't'): v = 0x010A; break; // gestaltCPUPentium4
    case FOURCC('c', 'p', 'u', 'f'): v = 0x0108; break; // gestaltCPUX86 family
    case FOURCC('s', 'y', 's', 'a'): v = 10; break;     // gestaltIntel
    case FOURCC('c', 'p', 'u', 's'):                    // gestaltCountOfCPUs
        v = (uint32_t)sysctl_u64("hw.logicalcpu", 4);
        break;
    case FOURCC('p', 'c', 'l', 'k'): // gestaltProcClkSpeed (Hz)
        v = cap32(sysctl_u64("hw.cpufrequency", 2400000000ull));
        break;
    case FOURCC('m', 'c', 'l', 'k'): // gestaltProcClkSpeedMHz
        v = (uint32_t)(sysctl_u64("hw.cpufrequency", 2400000000ull) / 1000000);
        break;
    case FOURCC('b', 'c', 'l', 'k'): v = cap32(sysctl_u64("hw.busfrequency", 100000000ull)); break;
    case FOURCC('p', 'g', 's', 'z'): v = 4096; break; // gestaltLogicalPageSize (i386)
    case FOURCC('o', 's', 'a', 't'): v = 0x7F; break; // gestaltOSAttr: all the usual bits
    case FOURCC('q', 't', 'i', 'm'): v = 0x07798000; break; // QuickTime 7.7.9
    case FOURCC('v', 'm', ' ', ' '): v = 1; break;   // gestaltVMAttr: VM present
    default:
        ok = false;
        break;
    }
    if (!ok) {
        LOGI("Gestalt: unknown selector '%c%c%c%c'", sel >> 24, (sel >> 16) & 0xff, (sel >> 8) & 0xff, sel & 0xff);
        RET((uint32_t)(int32_t)gestaltUndefSelectorErr);
        return;
    }
    if (resp) gwrite32(resp, v);
    RET(noErr);
}

// --- timing ---------------------------------------------------------------------------
THUNK(Microseconds) {
    UnsignedWide w;
    Microseconds(&w);
    gwrite32(ARG(0), w.lo);
    gwrite32(ARG(0) + 4, w.hi);
}

// AbsoluteTime is guest absolute time, i.e. nanoseconds (see time.c).
extern uint64_t guest_abs_time(void), guest_abs_to_host(uint64_t ns);
THUNK(UpTime) { RET64(guest_abs_time()); }
THUNK(AbsoluteToNanoseconds) { RET64(ARG64(0)); }
THUNK(NanosecondsToAbsolute) { RET64(ARG64(0)); }
THUNK(AbsoluteToDuration) {
    uint64_t h = guest_abs_to_host(ARG64(0));
    AbsoluteTime t = {.lo = (uint32_t)h, .hi = (uint32_t)(h >> 32)};
    RET((uint32_t)AbsoluteToDuration(t));
}

// Delay(numTicks, &finalTicks): ticks are 1/60 s.
THUNK(Delay) {
    uint32_t ticks = ARG(0);
    if (ticks) usleep((useconds_t)((uint64_t)ticks * 1000000 / 60));
    if (ARG(1)) {
        uint64_t ns = guest_abs_time();
        gwrite32(ARG(1), (uint32_t)(ns * 60 / 1000000000ull));
    }
}

THUNK(DebugStr) {
    gaddr_t s = ARG(0);
    if (s) LOGE("DebugStr: %.*s", gread8(s), (const char *)g2h(s + 1));
}

THUNK(Debugger) { LOGE("Debugger() called by guest"); }

THUNK(UpdateSystemActivity) { RET((uint32_t)(int32_t)UpdateSystemActivity((UInt8)ARG(0))); }

THUNK(CSCopyUserName) { RET_CF(CSCopyUserName(ARGBOOL(0))); }

// --- UTC utils -------------------------------------------------------------------------
THUNK(UCConvertCFAbsoluteTimeToLongDateTime) {
    LongDateTime ld = 0;
    OSStatus st = UCConvertCFAbsoluteTimeToLongDateTime(ARGD(0), &ld);
    if (ARG(2)) {
        gwrite32(ARG(2), (uint32_t)ld);
        gwrite32(ARG(2) + 4, (uint32_t)((uint64_t)ld >> 32));
    }
    RET((uint32_t)st);
}

THUNK(UCConvertLongDateTimeToCFAbsoluteTime) {
    CFAbsoluteTime t = 0;
    OSStatus st = UCConvertLongDateTimeToCFAbsoluteTime((LongDateTime)ARG64(0), &t);
    if (ARG(2)) memcpy(g2h(ARG(2)), &t, 8);
    RET((uint32_t)st);
}

THUNK(UCConvertSecondsToCFAbsoluteTime) {
    CFAbsoluteTime t = 0;
    OSStatus st = UCConvertSecondsToCFAbsoluteTime(ARG(0), &t);
    if (ARG(1)) memcpy(g2h(ARG(1)), &t, 8);
    RET((uint32_t)st);
}

THUNK(UCConvertCFAbsoluteTimeToSeconds) {
    UInt32 s = 0;
    OSStatus st = UCConvertCFAbsoluteTimeToSeconds(ARGD(0), &s);
    if (ARG(2)) gwrite32(ARG(2), s);
    RET((uint32_t)st);
}

// --- keyboard ---------------------------------------------------------------------------
// The layout data is position independent (offsets only), so a guest copy works.
THUNK(UCKeyTranslate) {
    UInt32 dead = ARG(6) ? gread32(ARG(6)) : 0;
    UniCharCount actual = 0;
    OSStatus st = UCKeyTranslate((const UCKeyboardLayout *)ARGP(0), (UInt16)ARG(1), (UInt16)ARG(2), ARG(3), ARG(4),
                                 ARG(5), &dead, (UniCharCount)ARG(7), &actual, (UniChar *)ARGP(9));
    if (ARG(6)) gwrite32(ARG(6), dead);
    if (ARG(8)) gwrite32(ARG(8), (uint32_t)actual);
    RET((uint32_t)st);
}

// --- Collection Manager -------------------------------------------------------------------
// Collection is an opaque host pointer: hobj handle.
THUNK(NewCollection) { RET(hobj_wrap(NewCollection())); }
THUNK(DisposeCollection) {
    Collection c = hobj_unwrap(ARG(0));
    if (c) DisposeCollection(c);
    hobj_forget(ARG(0));
}
THUNK(GetTaggedCollectionItem) {
    Collection c = hobj_unwrap(ARG(0));
    SInt32 size = ARG(3) ? (SInt32)gread32(ARG(3)) : 0;
    OSErr e = GetTaggedCollectionItem(c, ARG(1), ARGI(2), ARG(3) ? &size : NULL, ARGP(4));
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)size);
    RET((uint32_t)(int32_t)e);
}
THUNK(AddCollectionItem) {
    Collection c = hobj_unwrap(ARG(0));
    RET((uint32_t)(int32_t)AddCollectionItem(c, ARG(1), ARGI(2), ARGI(3), ARGP(4)));
}
THUNK(CountTaggedCollectionItems) { RET((uint32_t)CountTaggedCollectionItems(hobj_unwrap(ARG(0)), ARG(1))); }
