// CoreFoundation: base object API, allocators, booleans, numbers, dates, time
// zones, locales, formatters, UUIDs, errors and notification centers.
#include "cf_bridge.h"

#include <pthread.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Guest data symbol holding the guest handle of a host CF constant.
#define CF_CONST_AS(id, symname, expr)                                                                 \
    static void init_cfc_##id(gaddr_t a) { gwrite32(a, cfg((CFTypeRef)(expr))); }                \
    GDATA_AS(id, symname, 4, init_cfc_##id)
#define CF_CONST(sym) CF_CONST_AS(sym, "_" #sym, sym)

// --- allocators -------------------------------------------------------------------
static void init_zero(gaddr_t a) { gwrite32(a, 0); }
GDATA(kCFAllocatorDefault, 4, init_zero); // NULL in CF as well
CF_CONST(kCFAllocatorSystemDefault);
CF_CONST(kCFAllocatorMalloc);
CF_CONST(kCFAllocatorMallocZone);
CF_CONST(kCFAllocatorNull);
CF_CONST(kCFAllocatorUseContext);

THUNK(CFAllocatorGetDefault) { RET_CF(CFAllocatorGetDefault()); }
THUNK(CFAllocatorSetDefault) { CFAllocatorSetDefault(gcf_allocator(ARG(0))); }
THUNK(CFAllocatorAllocate) {
    // Allocations requested by the guest must be guest memory.
    CFAllocatorRef a = gcf_allocator(ARG(0));
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc || a == kCFAllocatorDefault)
        RET(gheap_alloc(ARG(1)));
    else
        RET(h2g(CFAllocatorAllocate(a, ARGIDX(1), ARG(2))));
}
THUNK(CFAllocatorDeallocate) {
    CFAllocatorRef a = gcf_allocator(ARG(0));
    if (!ARG(1) || a == kCFAllocatorNull) return;
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc) gheap_free(ARG(1));
    else CFAllocatorDeallocate(a, ARGP(1));
}
THUNK(CFAllocatorReallocate) {
    CFAllocatorRef a = gcf_allocator(ARG(0));
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc) { RET(gheap_realloc(ARG(1), ARG(2))); return; }
    else { RET(h2g(CFAllocatorReallocate(a, ARGP(1), ARGIDX(2), ARG(3)))); return; }
}
THUNK(CFAllocatorGetTypeID) { RET(CFAllocatorGetTypeID()); }

// --- base object API --------------------------------------------------------------------
THUNK(CFRetain) {
    gaddr_t g = ARG(0);
    if (!g) fatal("CFRetain(NULL) called by guest (return address %#x)", gread32(r->esp));
    if (!cf_is_guest_constant(g)) {
        CFTypeRef h = gcf(g);
        if (h) CFRetain(h);
    }
    RET(g);
}
THUNK(CFRelease) {
    gaddr_t g = ARG(0);
    if (!g) fatal("CFRelease(NULL) called by guest (return address %#x)", gread32(r->esp));
    if (cf_is_guest_constant(g)) return; // literals are immortal
    CFTypeRef h = gcf(g);
    if (h) CFRelease(h);
}
THUNK(CFAutorelease) {
    gaddr_t g = ARG(0);
    if (!cf_is_guest_constant(g)) {
        CFTypeRef h = gcf(g);
        if (h) CFAutorelease(h);
    }
    RET(g);
}
THUNK(CFMakeCollectable) { RET(ARG(0)); }
THUNK(CFGetRetainCount) {
    CFTypeRef h = ARGCF(0);
    RET(cf_is_guest_constant(ARG(0)) ? 0x7fffffff : h ? (uint32_t)CFGetRetainCount(h) : 0);
}
THUNK(CFGetTypeID) {
    CFTypeRef h = ARGCF(0);
    RET(h ? CFGetTypeID(h) : 0);
}
THUNK(CFCopyTypeIDDescription) { RET_CF(CFCopyTypeIDDescription(ARG(0))); }
THUNK(CFCopyDescription) {
    CFTypeRef h = ARGCF(0);
    RET_CF(h ? CFCopyDescription(h) : NULL);
}
THUNK(CFEqual) {
    CFTypeRef a = ARGCF(0), b = ARGCF(1);
    if (!a || !b) { RET_BOOL(a == b); return; }
    else { RET_BOOL(CFEqual(a, b)); return; }
}
THUNK(CFHash) {
    CFTypeRef h = ARGCF(0);
    RET(h ? (uint32_t)CFHash(h) : 0);
}
THUNK(CFGetAllocator) {
    CFTypeRef h = ARGCF(0);
    RET_CF(h ? CFGetAllocator(h) : NULL);
}
THUNK(CFShow) {
    CFTypeRef h = ARGCF(0);
    if (h) CFShow(h);
    else fprintf(stderr, "(null)\n");
}
THUNK(CFShowStr) {
    CFTypeRef h = ARGCF(0);
    if (h) CFShowStr(h);
}

// --- CFBoolean / CFNull ----------------------------------------------------------------------
CF_CONST(kCFBooleanTrue);
CF_CONST(kCFBooleanFalse);
CF_CONST(kCFNull);
THUNK(CFBooleanGetTypeID) { RET(CFBooleanGetTypeID()); }
THUNK(CFBooleanGetValue) {
    CFBooleanRef b = (CFBooleanRef)ARGCF(0);
    RET_BOOL(b && CFBooleanGetValue(b));
}
THUNK(CFNullGetTypeID) { RET(CFNullGetTypeID()); }

// --- CFNumber ------------------------------------------------------------------------------------
CF_CONST(kCFNumberPositiveInfinity);
CF_CONST(kCFNumberNegativeInfinity);
CF_CONST(kCFNumberNaN);

// Guest number types whose size differs on i386.
static CFNumberType gnumtype(uint32_t t) {
    switch (t) {
    case kCFNumberLongType:
    case kCFNumberCFIndexType:
    case kCFNumberNSIntegerType: return kCFNumberSInt32Type;
    case kCFNumberCGFloatType: return kCFNumberFloat32Type;
    default: return (CFNumberType)t;
    }
}
static uint32_t gnumsize(CFNumberType t) {
    switch (gnumtype(t)) {
    case kCFNumberSInt8Type:
    case kCFNumberCharType: return 1;
    case kCFNumberSInt16Type:
    case kCFNumberShortType: return 2;
    case kCFNumberSInt64Type:
    case kCFNumberFloat64Type:
    case kCFNumberLongLongType:
    case kCFNumberDoubleType: return 8;
    default: return 4;
    }
}

THUNK(CFNumberGetTypeID) { RET(CFNumberGetTypeID()); }
THUNK(CFNumberCreate) {
    if (!ARG(2)) { RET(0); return; }
    RET_CF(CFNumberCreate(gcf_allocator(ARG(0)), gnumtype(ARG(1)), ARGP(2)));
}
THUNK(CFNumberGetValue) {
    CFNumberRef n = (CFNumberRef)ARGCF(0);
    if (!n || !ARG(2)) { RET(0); return; }
    RET_BOOL(CFNumberGetValue(n, gnumtype(ARG(1)), ARGP(2)));
}
THUNK(CFNumberGetType) {
    CFNumberRef n = (CFNumberRef)ARGCF(0);
    RET(n ? CFNumberGetType(n) : 0);
}
THUNK(CFNumberGetByteSize) {
    CFNumberRef n = (CFNumberRef)ARGCF(0);
    RET(n ? gnumsize(CFNumberGetType(n)) : 0);
}
THUNK(CFNumberIsFloatType) {
    CFNumberRef n = (CFNumberRef)ARGCF(0);
    RET_BOOL(n && CFNumberIsFloatType(n));
}
THUNK(CFNumberCompare) {
    CFNumberRef a = (CFNumberRef)ARGCF(0), b = (CFNumberRef)ARGCF(1);
    RET((uint32_t)CFNumberCompare(a, b, NULL));
}

// --- CFNumberFormatter ---------------------------------------------------------------------
CF_CONST(kCFNumberFormatterCurrencyCode);
CF_CONST(kCFNumberFormatterMaxFractionDigits);
CF_CONST(kCFNumberFormatterMinFractionDigits);
CF_CONST(kCFNumberFormatterCurrencySymbol);
CF_CONST(kCFNumberFormatterDecimalSeparator);
CF_CONST(kCFNumberFormatterGroupingSeparator);
THUNK(CFNumberFormatterCreate) {
    RET_CF(CFNumberFormatterCreate(gcf_allocator(ARG(0)), (CFLocaleRef)ARGCF(1), ARGIDX(2)));
}
THUNK(CFNumberFormatterCreateStringWithNumber) {
    RET_CF(CFNumberFormatterCreateStringWithNumber(gcf_allocator(ARG(0)), (CFNumberFormatterRef)ARGCF(1),
                                                   (CFNumberRef)ARGCF(2)));
}
THUNK(CFNumberFormatterCreateStringWithValue) {
    if (!ARG(3)) { RET(0); return; }
    RET_CF(CFNumberFormatterCreateStringWithValue(gcf_allocator(ARG(0)), (CFNumberFormatterRef)ARGCF(1),
                                                  gnumtype(ARG(2)), ARGP(3)));
}
THUNK(CFNumberFormatterSetProperty) {
    CFNumberFormatterSetProperty((CFNumberFormatterRef)ARGCF(0), ARGSTR(1), ARGCF(2));
}
THUNK(CFNumberFormatterCopyProperty) {
    RET_CF(CFNumberFormatterCopyProperty((CFNumberFormatterRef)ARGCF(0), ARGSTR(1)));
}
THUNK(CFNumberFormatterSetFormat) { CFNumberFormatterSetFormat((CFNumberFormatterRef)ARGCF(0), ARGSTR(1)); }

// --- time ------------------------------------------------------------------------------------
static void init_since1970(gaddr_t a) {
    double d = kCFAbsoluteTimeIntervalSince1970;
    memcpy(g2h(a), &d, 8);
}
static void init_since1904(gaddr_t a) {
    double d = kCFAbsoluteTimeIntervalSince1904;
    memcpy(g2h(a), &d, 8);
}
GDATA(kCFAbsoluteTimeIntervalSince1970, 8, init_since1970);
GDATA(kCFAbsoluteTimeIntervalSince1904, 8, init_since1904);

THUNK(CFAbsoluteTimeGetCurrent) { RETD(CFAbsoluteTimeGetCurrent()); }

// CFGregorianDate (i386): SInt32 year; SInt8 month, day, hour, minute; double second (offset 8) = 16 bytes.
static void put_ggreg(gaddr_t g, CFGregorianDate d) {
    gwrite32(g, (uint32_t)d.year);
    gwrite8(g + 4, (uint8_t)d.month);
    gwrite8(g + 5, (uint8_t)d.day);
    gwrite8(g + 6, (uint8_t)d.hour);
    gwrite8(g + 7, (uint8_t)d.minute);
    memcpy(g2h(g + 8), &d.second, 8);
}
static CFGregorianDate get_ggreg(struct guest_regs *r, int i) {
    CFGregorianDate d;
    d.year = ARGI(i);
    uint32_t w = ARG(i + 1);
    d.month = (SInt8)(w & 0xff);
    d.day = (SInt8)((w >> 8) & 0xff);
    d.hour = (SInt8)((w >> 16) & 0xff);
    d.minute = (SInt8)(w >> 24);
    d.second = ARGD(i + 2);
    return d;
}

THUNK(CFAbsoluteTimeGetGregorianDate) { // hidden struct return
    gaddr_t out = ARG(0);
    put_ggreg(out, CFAbsoluteTimeGetGregorianDate(ARGD(1), (CFTimeZoneRef)ARGCF(3)));
    r->pop = 4;
    RET(out);
}
THUNK(CFGregorianDateGetAbsoluteTime) { RETD(CFGregorianDateGetAbsoluteTime(get_ggreg(r, 0), (CFTimeZoneRef)ARGCF(4))); }
THUNK(CFGregorianDateIsValid) { RET_BOOL(CFGregorianDateIsValid(get_ggreg(r, 0), ARG(4))); }
THUNK(CFAbsoluteTimeGetDayOfWeek) { RET(CFAbsoluteTimeGetDayOfWeek(ARGD(0), (CFTimeZoneRef)ARGCF(2))); }
THUNK(CFAbsoluteTimeGetDayOfYear) { RET(CFAbsoluteTimeGetDayOfYear(ARGD(0), (CFTimeZoneRef)ARGCF(2))); }
THUNK(CFAbsoluteTimeGetWeekOfYear) { RET(CFAbsoluteTimeGetWeekOfYear(ARGD(0), (CFTimeZoneRef)ARGCF(2))); }

THUNK(CFDateGetTypeID) { RET(CFDateGetTypeID()); }
THUNK(CFDateCreate) { RET_CF(CFDateCreate(gcf_allocator(ARG(0)), ARGD(1))); }
THUNK(CFDateGetAbsoluteTime) {
    CFDateRef d = (CFDateRef)ARGCF(0);
    RETD(d ? CFDateGetAbsoluteTime(d) : 0.0);
}
THUNK(CFDateGetTimeIntervalSinceDate) {
    RETD(CFDateGetTimeIntervalSinceDate((CFDateRef)ARGCF(0), (CFDateRef)ARGCF(1)));
}
THUNK(CFDateCompare) { RET((uint32_t)CFDateCompare((CFDateRef)ARGCF(0), (CFDateRef)ARGCF(1), NULL)); }

// --- CFDateFormatter ------------------------------------------------------------------------
THUNK(CFDateFormatterCreate) {
    RET_CF(CFDateFormatterCreate(gcf_allocator(ARG(0)), (CFLocaleRef)ARGCF(1), ARGIDX(2), ARGIDX(3)));
}
THUNK(CFDateFormatterCreateStringWithAbsoluteTime) {
    RET_CF(CFDateFormatterCreateStringWithAbsoluteTime(gcf_allocator(ARG(0)), (CFDateFormatterRef)ARGCF(1), ARGD(2)));
}
THUNK(CFDateFormatterCreateStringWithDate) {
    RET_CF(CFDateFormatterCreateStringWithDate(gcf_allocator(ARG(0)), (CFDateFormatterRef)ARGCF(1),
                                               (CFDateRef)ARGCF(2)));
}
THUNK(CFDateFormatterSetFormat) { CFDateFormatterSetFormat((CFDateFormatterRef)ARGCF(0), ARGSTR(1)); }
THUNK(CFDateFormatterSetProperty) { CFDateFormatterSetProperty((CFDateFormatterRef)ARGCF(0), ARGSTR(1), ARGCF(2)); }

// --- CFTimeZone --------------------------------------------------------------------------------
THUNK(CFTimeZoneGetTypeID) { RET(CFTimeZoneGetTypeID()); }
THUNK(CFTimeZoneCopyDefault) { RET_CF(CFTimeZoneCopyDefault()); }
THUNK(CFTimeZoneCopySystem) { RET_CF(CFTimeZoneCopySystem()); }
THUNK(CFTimeZoneResetSystem) { CFTimeZoneResetSystem(); }
THUNK(CFTimeZoneCreateWithName) { RET_CF(CFTimeZoneCreateWithName(gcf_allocator(ARG(0)), ARGSTR(1), ARGBOOL(2))); }
THUNK(CFTimeZoneCreateWithTimeIntervalFromGMT) {
    RET_CF(CFTimeZoneCreateWithTimeIntervalFromGMT(gcf_allocator(ARG(0)), ARGD(1)));
}
THUNK(CFTimeZoneGetName) { RET_CF(CFTimeZoneGetName((CFTimeZoneRef)ARGCF(0))); }
THUNK(CFTimeZoneCopyAbbreviation) { RET_CF(CFTimeZoneCopyAbbreviation((CFTimeZoneRef)ARGCF(0), ARGD(1))); }
THUNK(CFTimeZoneGetSecondsFromGMT) { RETD(CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)ARGCF(0), ARGD(1))); }
THUNK(CFTimeZoneIsDaylightSavingTime) { RET_BOOL(CFTimeZoneIsDaylightSavingTime((CFTimeZoneRef)ARGCF(0), ARGD(1))); }
THUNK(CFTimeZoneGetDaylightSavingTimeOffset) {
    RETD(CFTimeZoneGetDaylightSavingTimeOffset((CFTimeZoneRef)ARGCF(0), ARGD(1)));
}
THUNK(CFTimeZoneGetNextDaylightSavingTimeTransition) {
    RETD(CFTimeZoneGetNextDaylightSavingTimeTransition((CFTimeZoneRef)ARGCF(0), ARGD(1)));
}
THUNK(CFTimeZoneCopyLocalizedName) {
    RET_CF(CFTimeZoneCopyLocalizedName((CFTimeZoneRef)ARGCF(0), ARGIDX(1), (CFLocaleRef)ARGCF(2)));
}

// --- CFLocale ----------------------------------------------------------------------------------
CF_CONST(kCFLocaleCountryCode);
CF_CONST(kCFLocaleCurrencyCode);
CF_CONST(kCFLocaleLanguageCode);
CF_CONST(kCFLocaleIdentifier);
CF_CONST(kCFLocaleDecimalSeparator);
CF_CONST(kCFLocaleGroupingSeparator);
CF_CONST(kCFLocaleCurrencySymbol);
CF_CONST(kCFLocaleUsesMetricSystem);
THUNK(CFLocaleGetTypeID) { RET(CFLocaleGetTypeID()); }
THUNK(CFLocaleCopyCurrent) { RET_CF(CFLocaleCopyCurrent()); }
THUNK(CFLocaleGetSystem) { RET_CF(CFLocaleGetSystem()); }
THUNK(CFLocaleCreate) { RET_CF(CFLocaleCreate(gcf_allocator(ARG(0)), ARGSTR(1))); }
THUNK(CFLocaleGetValue) { RET_CF(CFLocaleGetValue((CFLocaleRef)ARGCF(0), ARGSTR(1))); }
THUNK(CFLocaleGetIdentifier) { RET_CF(CFLocaleGetIdentifier((CFLocaleRef)ARGCF(0))); }
THUNK(CFLocaleCopyPreferredLanguages) { RET_CF(CFLocaleCopyPreferredLanguages()); }
THUNK(CFLocaleCopyDisplayNameForPropertyValue) {
    RET_CF(CFLocaleCopyDisplayNameForPropertyValue((CFLocaleRef)ARGCF(0), ARGSTR(1), ARGSTR(2)));
}

// --- CFUUID -------------------------------------------------------------------------------------
THUNK(CFUUIDGetTypeID) { RET(CFUUIDGetTypeID()); }
THUNK(CFUUIDCreate) { RET_CF(CFUUIDCreate(gcf_allocator(ARG(0)))); }
THUNK(CFUUIDCreateString) { RET_CF(CFUUIDCreateString(gcf_allocator(ARG(0)), (CFUUIDRef)ARGCF(1))); }
THUNK(CFUUIDCreateFromString) { RET_CF(CFUUIDCreateFromString(gcf_allocator(ARG(0)), ARGSTR(1))); }
// The 16 UInt8 arguments each take a 4-byte stack slot.
#define UUID_ARGS(base)                                                                            \
    (UInt8) ARG(base), (UInt8)ARG(base + 1), (UInt8)ARG(base + 2), (UInt8)ARG(base + 3),           \
        (UInt8)ARG(base + 4), (UInt8)ARG(base + 5), (UInt8)ARG(base + 6), (UInt8)ARG(base + 7),    \
        (UInt8)ARG(base + 8), (UInt8)ARG(base + 9), (UInt8)ARG(base + 10), (UInt8)ARG(base + 11),  \
        (UInt8)ARG(base + 12), (UInt8)ARG(base + 13), (UInt8)ARG(base + 14), (UInt8)ARG(base + 15)
THUNK(CFUUIDGetConstantUUIDWithBytes) { RET_CF(CFUUIDGetConstantUUIDWithBytes(gcf_allocator(ARG(0)), UUID_ARGS(1))); }
THUNK(CFUUIDCreateWithBytes) { RET_CF(CFUUIDCreateWithBytes(gcf_allocator(ARG(0)), UUID_ARGS(1))); }
THUNK(CFUUIDGetUUIDBytes) { // CFUUIDBytes (16 bytes) via hidden pointer
    gaddr_t out = ARG(0);
    CFUUIDBytes b = CFUUIDGetUUIDBytes((CFUUIDRef)ARGCF(1));
    memcpy(g2h(out), &b, 16);
    r->pop = 4;
    RET(out);
}
THUNK(CFUUIDCreateFromUUIDBytes) { // CFUUIDBytes by value: 4 slots
    CFUUIDBytes b;
    memcpy(&b, g2h(r->esp + 8), 16);
    RET_CF(CFUUIDCreateFromUUIDBytes(gcf_allocator(ARG(0)), b));
}

// --- CFError -------------------------------------------------------------------------------------
THUNK(CFErrorGetTypeID) { RET(CFErrorGetTypeID()); }
THUNK(CFErrorCopyFailureReason) { RET_CF(CFErrorCopyFailureReason((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorCopyDescription) { RET_CF(CFErrorCopyDescription((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorCopyRecoverySuggestion) { RET_CF(CFErrorCopyRecoverySuggestion((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorCopyUserInfo) { RET_CF(CFErrorCopyUserInfo((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorGetCode) { RET((uint32_t)CFErrorGetCode((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorGetDomain) { RET_CF(CFErrorGetDomain((CFErrorRef)ARGCF(0))); }
THUNK(CFErrorCreate) {
    RET_CF(CFErrorCreate(gcf_allocator(ARG(0)), ARGSTR(1), ARGIDX(2), (CFDictionaryRef)ARGCF(3)));
}

// --- CFNotificationCenter ------------------------------------------------------------------------------
// The host observer pointer is a record holding the guest callback and observer.
struct gobserver {
    struct gobserver *next;
    CFNotificationCenterRef center;
    gaddr_t callback, observer;
};
static struct gobserver *observers;
static pthread_mutex_t observer_lock = PTHREAD_MUTEX_INITIALIZER;

// 'object' arguments are CF objects for the distributed/Darwin centers but may
// be arbitrary pointers for the local center.
static const void *gobj_in(gaddr_t g) {
    if (hobj_is_handle(g) || cf_is_guest_constant(g)) return gcf(g);
    return (const void *)(uintptr_t)g;
}
static uint32_t gobj_out(const void *p) {
    if ((uintptr_t)p >> 32) return cfg(p);
    return (uint32_t)(uintptr_t)p;
}

static void notification_cb(CFNotificationCenterRef center, void *observer, CFNotificationName name,
                            const void *object, CFDictionaryRef userInfo) {
    struct gobserver *o = observer;
    uint32_t args[5] = {cfg(center), o->observer, cfg(name), gobj_out(object), cfg(userInfo)};
    cpu_call(o->callback, 5, args);
}

THUNK(CFNotificationCenterGetTypeID) { RET(CFNotificationCenterGetTypeID()); }
THUNK(CFNotificationCenterGetLocalCenter) { RET_CF(CFNotificationCenterGetLocalCenter()); }
THUNK(CFNotificationCenterGetDistributedCenter) { RET_CF(CFNotificationCenterGetDistributedCenter()); }
THUNK(CFNotificationCenterGetDarwinNotifyCenter) { RET_CF(CFNotificationCenterGetDarwinNotifyCenter()); }
THUNK(CFNotificationCenterAddObserver) {
    CFNotificationCenterRef c = (CFNotificationCenterRef)ARGCF(0);
    struct gobserver *o = calloc(1, sizeof *o);
    o->center = c;
    o->observer = ARG(1);
    o->callback = ARG(2);
    pthread_mutex_lock(&observer_lock);
    o->next = observers;
    observers = o;
    pthread_mutex_unlock(&observer_lock);
    LOGD("CFNotificationCenterAddObserver(%p, observer %#x, cb %#x)", c, o->observer, o->callback);
    CFNotificationCenterAddObserver(c, o, notification_cb, ARGSTR(3), gobj_in(ARG(4)), ARGIDX(5));
}
static void remove_observers(CFNotificationCenterRef c, gaddr_t observer, gaddr_t name, gaddr_t object, bool every) {
    pthread_mutex_lock(&observer_lock);
    for (struct gobserver *o = observers; o; o = o->next) {
        if (o->center != c || o->observer != observer) continue;
        if (every) CFNotificationCenterRemoveEveryObserver(c, o);
        else CFNotificationCenterRemoveObserver(c, o, gcf_string(name), gobj_in(object));
        // Records are kept (callbacks may still be in flight); they are tiny.
    }
    pthread_mutex_unlock(&observer_lock);
}
THUNK(CFNotificationCenterRemoveObserver) {
    remove_observers((CFNotificationCenterRef)ARGCF(0), ARG(1), ARG(2), ARG(3), false);
}
THUNK(CFNotificationCenterRemoveEveryObserver) {
    remove_observers((CFNotificationCenterRef)ARGCF(0), ARG(1), 0, 0, true);
}
THUNK(CFNotificationCenterPostNotification) {
    CFNotificationCenterPostNotification((CFNotificationCenterRef)ARGCF(0), ARGSTR(1), gobj_in(ARG(2)),
                                         (CFDictionaryRef)ARGCF(3), ARGBOOL(4));
}
THUNK(CFNotificationCenterPostNotificationWithOptions) {
    CFNotificationCenterPostNotificationWithOptions((CFNotificationCenterRef)ARGCF(0), ARGSTR(1), gobj_in(ARG(2)),
                                                    (CFDictionaryRef)ARGCF(3), ARG(4));
}

// --- more constants -----------------------------------------------------------------------------
CF_CONST(kCFDateFormatterTimeZone);
CF_CONST(kCFDateFormatterCalendar);
CF_CONST(kCFDateFormatterCalendarName);
CF_CONST(kCFDateFormatterIsLenient);
CF_CONST(kCFDateFormatterDefaultFormat);
CF_CONST(kCFDateFormatterAMSymbol);
CF_CONST(kCFDateFormatterPMSymbol);
CF_CONST(kCFNumberFormatterRoundingMode);
CF_CONST(kCFNumberFormatterMaxIntegerDigits);
CF_CONST(kCFNumberFormatterMinIntegerDigits);
CF_CONST(kCFNumberFormatterUseGroupingSeparator);
CF_CONST(kCFNumberFormatterInternationalCurrencySymbol);
CF_CONST(kCFLocaleScriptCode);
CF_CONST(kCFLocaleVariantCode);
CF_CONST(kCFLocaleCalendarIdentifier);
CF_CONST(kCFLocaleMeasurementSystem);
CF_CONST(kCFGregorianCalendar);
CF_CONST(kCFErrorDomainPOSIX);
CF_CONST(kCFErrorDomainOSStatus);
CF_CONST(kCFErrorDomainMach);
CF_CONST(kCFErrorDomainCocoa);
CF_CONST(kCFErrorLocalizedDescriptionKey);
CF_CONST(kCFErrorLocalizedFailureReasonKey);
CF_CONST(kCFErrorUnderlyingErrorKey);
CF_CONST(kCFErrorDescriptionKey);
CF_CONST(kCFTimeZoneSystemTimeZoneDidChangeNotification);
CF_CONST(kCFLocaleCurrentLocaleDidChangeNotification);
CF_CONST(kCFStreamPropertyDataWritten);
CF_CONST(kCFStreamPropertyFileCurrentOffset);
CF_CONST(kCFStreamPropertyAppendToFile);
CF_CONST(kCFStreamPropertySocketRemoteHostName);
CF_CONST(kCFStreamPropertySocketRemotePortNumber);
CF_CONST(kCFURLNameKey);
CF_CONST(kCFURLIsDirectoryKey);
CF_CONST(kCFURLFileSizeKey);
CF_CONST(kCFURLContentModificationDateKey);
CF_CONST(kCFURLVolumeNameKey);
CF_CONST(kCFURLLocalizedNameKey);
