// CoreFoundation runtime services: run loop timers/observers/sources, bundles,
// preferences, URLs, dates, XML trees, CFTree, custom allocators/callbacks.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

_Static_assert(sizeof(CFXMLElementInfo) == 12, "i386 CFXMLElementInfo");
_Static_assert(sizeof(CFRunLoopSourceContext) == 40, "i386 source context");
_Static_assert(sizeof(CFStreamError) == 8, "i386 CFStreamError");

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static char buf[1024];
static const char *cstr(CFStringRef s) {
    if (!s) return "(null)";
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return "(conversion failed)";
    return buf;
}

// --- run loop ------------------------------------------------------------------------
static int timer_fired, info_retains, info_releases, observer_calls, source_performs;
static const void *info_retain(const void *info) { info_retains++; return info; }
static void info_release(const void *info) { info_releases++; }
static void timer_cb(CFRunLoopTimerRef t, void *info) {
    int *counter = info;
    (*counter)++;
    if (*counter == 3) CFRunLoopTimerInvalidate(t);
}
static void observer_cb(CFRunLoopObserverRef o, CFRunLoopActivity a, void *info) { observer_calls++; }
static void source_perform(void *info) { source_performs += (int)(intptr_t)info; }

static void test_runloop(void) {
    int counter = 0;
    CFRunLoopTimerContext ctx = {0, &counter, info_retain, info_release, NULL};
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.01, 0.01, 0, 0, timer_cb, &ctx);
    CHECK(info_retains == 1);
    CFRunLoopTimerContext back;
    memset(&back, 0, sizeof back);
    CFRunLoopTimerGetContext(t, &back);
    CHECK(back.info == &counter && back.retain == info_retain);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRunLoopObserverContext octx = {0, NULL, NULL, NULL, NULL};
    CFRunLoopObserverRef o =
        CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, 0, observer_cb, &octx);
    CFRunLoopAddObserver(CFRunLoopGetCurrent(), o, kCFRunLoopDefaultMode);
    CFRunLoopSourceContext sctx;
    memset(&sctx, 0, sizeof sctx);
    sctx.info = (void *)5;
    sctx.perform = source_perform;
    CFRunLoopSourceRef src = CFRunLoopSourceCreate(NULL, 0, &sctx);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    CFRunLoopSourceSignal(src);
    for (int i = 0; i < 50 && counter < 3; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    printf("timer fired %d times, observer %d calls, source perform sum %d\n", counter, observer_calls, source_performs);
    CHECK(counter == 3);
    CHECK(source_performs == 5);
    CHECK(observer_calls > 0);
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != 0);
    CFRunLoopSourceInvalidate(src);
    CFRelease(src);
    CFRunLoopObserverInvalidate(o);
    CFRelease(o);
    CFRelease(t);
    CHECK(info_releases == 1);
    CHECK(CFRunLoopGetMain() == CFRunLoopGetCurrent());
}

// --- bundle / preferences / urls ------------------------------------------------------------------
static void test_bundle(void) {
    CFBundleRef b = CFBundleGetMainBundle();
    CHECK(b != NULL);
    CFURLRef u = CFBundleCopyBundleURL(b);
    CHECK(u != NULL);
    CFStringRef path = CFURLCopyFileSystemPath(u, kCFURLPOSIXPathStyle);
    printf("main bundle: %s\n", cstr(path));
    CFRelease(path);
    CFRelease(u);
    CFDictionaryRef info = CFBundleGetInfoDictionary(b);
    printf("info dictionary: %ld keys, id %s\n", info ? (long)CFDictionaryGetCount(info) : -1L,
           cstr(CFBundleGetIdentifier(b)));
    CFURLRef missing = CFBundleCopyResourceURL(b, CFSTR("does-not-exist"), CFSTR("txt"), NULL);
    CHECK(missing == NULL);
    CFURLRef sysurl = CFURLCreateWithFileSystemPath(NULL, CFSTR("/System/Library/Frameworks/OpenGL.framework"),
                                                    kCFURLPOSIXPathStyle, true);
    CFBundleRef gl = CFBundleCreate(NULL, sysurl);
    CHECK(gl != NULL);
    CHECK(CFEqual(CFBundleGetIdentifier(gl), CFSTR("com.apple.opengl")));
    void *fn = CFBundleGetFunctionPointerForName(gl, CFSTR("CFRelease")); // any host thunk
    CHECK(fn != NULL);
    CFRelease(gl);
    CFRelease(sysurl);
}

static void test_prefs(void) {
    CFStringRef app = CFSTR("com.dxhost.cftest");
    int v = 1234;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFPreferencesSetAppValue(CFSTR("TestNumber"), n, app);
    CFPreferencesSetAppValue(CFSTR("TestBool"), kCFBooleanTrue, app);
    CHECK(CFPreferencesAppSynchronize(app));
    CFPropertyListRef back = CFPreferencesCopyAppValue(CFSTR("TestNumber"), app);
    CHECK(back && CFEqual(back, n));
    if (back) CFRelease(back);
    Boolean valid = false;
    CHECK(CFPreferencesGetAppBooleanValue(CFSTR("TestBool"), app, &valid) && valid);
    CHECK(CFPreferencesGetAppIntegerValue(CFSTR("TestNumber"), app, &valid) == 1234 && valid);
    CFPreferencesGetAppBooleanValue(CFSTR("NoSuchKey"), app, &valid);
    CHECK(!valid);
    CFPreferencesSetAppValue(CFSTR("TestNumber"), NULL, app);
    CFPreferencesSetAppValue(CFSTR("TestBool"), NULL, app);
    CFPreferencesAppSynchronize(app);
    CFRelease(n);
}

static void test_url(void) {
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, CFSTR("/tmp/some dir/file.txt"), kCFURLPOSIXPathStyle, false);
    char path[256];
    CHECK(CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path));
    CHECK(strcmp(path, "/tmp/some dir/file.txt") == 0);
    CFStringRef last = CFURLCopyLastPathComponent(u);
    CHECK(CFEqual(last, CFSTR("file.txt")));
    CFRelease(last);
    CFURLRef dir = CFURLCreateCopyDeletingLastPathComponent(NULL, u);
    CHECK(CFURLHasDirectoryPath(dir));
    CFURLRef noext = CFURLCreateCopyDeletingPathExtension(NULL, u);
    CFStringRef s = CFURLGetString(noext);
    printf("url: %s\n", cstr(s));
    CHECK(strcmp(cstr(s), "file:///tmp/some%20dir/file") == 0);
    CFURLRef web = CFURLCreateWithString(NULL, CFSTR("http://www.feralinteractive.com:8080/x?y=1"), NULL);
    CFStringRef host = CFURLCopyHostName(web);
    CHECK(CFEqual(host, CFSTR("www.feralinteractive.com")));
    CHECK(CFURLGetPortNumber(web) == 8080);
    CFRelease(host);
    CFStringRef esc = CFURLCreateStringByAddingPercentEscapes(NULL, CFSTR("a b&c"), NULL, CFSTR("&"),
                                                              kCFStringEncodingUTF8);
    CHECK(strcmp(cstr(esc), "a%20b%26c") == 0);
    CFRelease(esc);
    // FSRef round trip
    FSRef ref;
    CFURLRef tmp = CFURLCreateWithFileSystemPath(NULL, CFSTR("/tmp"), kCFURLPOSIXPathStyle, true);
    CHECK(CFURLGetFSRef(tmp, &ref));
    CFURLRef back = CFURLCreateFromFSRef(NULL, &ref);
    CHECK(back != NULL);
    if (back) {
        CFURLGetFileSystemRepresentation(back, true, (UInt8 *)path, sizeof path);
        printf("fsref round trip: %s\n", path);
        CHECK(strstr(path, "tmp") != NULL);
        CFRelease(back);
    }
    CFRelease(tmp);
    CFRelease(web);
    CFRelease(noext);
    CFRelease(dir);
    CFRelease(u);
}

static void test_dates(void) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CHECK(now > 400000000.0); // after 2013
    CFTimeZoneRef gmt = CFTimeZoneCreateWithTimeIntervalFromGMT(NULL, 0);
    CFGregorianDate g = CFAbsoluteTimeGetGregorianDate(0.0, gmt);
    CHECK(g.year == 2001 && g.month == 1 && g.day == 1 && g.hour == 0 && g.second == 0.0);
    CFGregorianDate g2 = {2011, 8, 23, 12, 30, 15.5};
    CFAbsoluteTime t = CFGregorianDateGetAbsoluteTime(g2, gmt);
    CFGregorianDate g3 = CFAbsoluteTimeGetGregorianDate(t, gmt);
    CHECK(g3.year == 2011 && g3.month == 8 && g3.day == 23 && g3.minute == 30 && g3.second == 15.5);
    CHECK(CFAbsoluteTimeGetDayOfWeek(t, gmt) == 2); // Tuesday
    CFDateRef d = CFDateCreate(NULL, t);
    CHECK(CFDateGetAbsoluteTime(d) == t);
    CHECK(CFTimeZoneGetSecondsFromGMT(gmt, t) == 0.0);
    CFLocaleRef loc = CFLocaleCreate(NULL, CFSTR("en_US"));
    CFDateFormatterRef f = CFDateFormatterCreate(NULL, loc, kCFDateFormatterNoStyle, kCFDateFormatterNoStyle);
    CFDateFormatterSetFormat(f, CFSTR("yyyy-MM-dd HH:mm"));
    CFDateFormatterSetProperty(f, kCFDateFormatterTimeZone, gmt);
    CFStringRef s = CFDateFormatterCreateStringWithDate(NULL, f, d);
    CHECK(strcmp(cstr(s), "2011-08-23 12:30") == 0);
    CHECK(CFEqual(CFLocaleGetValue(loc, kCFLocaleCountryCode), CFSTR("US")));
    CFNumberFormatterRef nf = CFNumberFormatterCreate(NULL, loc, kCFNumberFormatterDecimalStyle);
    double dv = 1234.5;
    CFStringRef ns = CFNumberFormatterCreateStringWithValue(NULL, nf, kCFNumberDoubleType, &dv);
    printf("number formatter: %s\n", cstr(ns));
    CHECK(strcmp(cstr(ns), "1,234.5") == 0);
    CFUUIDRef uu = CFUUIDGetConstantUUIDWithBytes(NULL, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    CFUUIDBytes ub = CFUUIDGetUUIDBytes(uu);
    CHECK(ub.byte0 == 1 && ub.byte15 == 16);
    CFStringRef us = CFUUIDCreateString(NULL, uu);
    CHECK(strcmp(cstr(us), "01020304-0506-0708-090A-0B0C0D0E0F10") == 0);
    CFRelease(us);
    CFRelease(ns);
    CFRelease(nf);
    CFRelease(s);
    CFRelease(f);
    CFRelease(loc);
    CFRelease(d);
    CFRelease(gmt);
}

// --- XML / tree -----------------------------------------------------------------------------------
static int child_count;
static void count_child(const void *v, void *ctx) { child_count++; }

static void test_xml(void) {
    const char *xml = "<?xml version=\"1.0\"?><fmod><device name=\"out\" rate=\"48000\"/><x>text</x></fmod>";
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)xml, (CFIndex)strlen(xml));
    CFXMLTreeRef tree = CFXMLTreeCreateFromData(NULL, data, NULL, kCFXMLParserSkipWhitespace, kCFXMLNodeCurrentVersion);
    CHECK(tree != NULL);
    if (!tree) return;
    CFXMLNodeRef root = CFXMLTreeGetNode(tree);
    CHECK(CFXMLNodeGetTypeCode(root) == kCFXMLNodeTypeDocument);
    CFIndex n = CFTreeGetChildCount(tree);
    CFXMLTreeRef elem = NULL;
    for (CFIndex i = 0; i < n; i++) {
        CFXMLTreeRef c = CFTreeGetChildAtIndex(tree, i);
        if (CFXMLNodeGetTypeCode(CFXMLTreeGetNode(c)) == kCFXMLNodeTypeElement) elem = c;
    }
    CHECK(elem != NULL);
    if (elem) {
        CHECK(CFEqual(CFXMLNodeGetString(CFXMLTreeGetNode(elem)), CFSTR("fmod")));
        CHECK(CFTreeGetParent(elem) == tree);
        CHECK(CFTreeGetChildCount(elem) == 2);
        child_count = 0;
        CFTreeApplyFunctionToChildren(elem, count_child, NULL);
        CHECK(child_count == 2);
        CFXMLTreeRef dev = CFTreeGetFirstChild(elem);
        CFXMLNodeRef dn = CFXMLTreeGetNode(dev);
        const CFXMLElementInfo *ei = CFXMLNodeGetInfoPtr(dn);
        CHECK(ei && ei->isEmpty);
        CHECK(ei && CFEqual(CFDictionaryGetValue(ei->attributes, CFSTR("rate")), CFSTR("48000")));
        CHECK(ei && CFArrayGetCount(ei->attributeOrder) == 2);
    }
    CFDataRef out = CFXMLTreeCreateXMLData(NULL, tree);
    CHECK(out && CFDataGetLength(out) > 20);
    printf("xml out: %.*s\n", (int)CFDataGetLength(out), CFDataGetBytePtr(out));
    CFRelease(out);
    CFRelease(tree);
    CFRelease(data);

    // Guest-built XML node.
    CFXMLElementInfo info = {NULL, NULL, true};
    CFXMLNodeRef node = CFXMLNodeCreate(NULL, kCFXMLNodeTypeElement, CFSTR("leaf"), &info, kCFXMLNodeCurrentVersion);
    CHECK(node && CFEqual(CFXMLNodeGetString(node), CFSTR("leaf")));
    CFRelease(node);

    // Plain CFTree with a guest context.
    static int tree_releases;
    CFTreeContext ctx = {0, (void *)7, NULL, (void (*)(const void *))0, NULL};
    CFTreeRef t = CFTreeCreate(NULL, &ctx);
    CFTreeRef c1 = CFTreeCreate(NULL, &ctx);
    CFTreeAppendChild(t, c1);
    CFTreeContext back;
    CFTreeGetContext(c1, &back);
    CHECK(back.info == (void *)7);
    CHECK(CFTreeGetChildCount(t) == 1 && CFTreeGetParent(c1) == t);
    CFTreeRemove(c1);
    CHECK(CFTreeGetChildCount(t) == 0);
    CFRelease(c1);
    CFRelease(t);
    (void)tree_releases;
}

// --- custom allocator / callbacks ---------------------------------------------------------------------
static int allocs, deallocs;
static void *my_alloc(CFIndex size, CFOptionFlags hint, void *info) { allocs++; return malloc(size); }
static void my_dealloc(void *p, void *info) { deallocs++; free(p); }

typedef struct { int key; } my_key;
static Boolean key_equal(const void *a, const void *b) { return ((const my_key *)a)->key == ((const my_key *)b)->key; }
static CFHashCode key_hash(const void *a) { return (CFHashCode)((const my_key *)a)->key; }

static void test_custom(void) {
    CFAllocatorContext ac = {0, NULL, NULL, NULL, NULL, my_alloc, NULL, my_dealloc, NULL};
    CFAllocatorRef a = CFAllocatorCreate(NULL, &ac);
    CFStringRef s = CFStringCreateWithCString(a, "allocated by the guest allocator, long enough to avoid tagging",
                                              kCFStringEncodingASCII);
    CHECK(allocs > 0);
    CHECK(CFStringGetLength(s) == 62);
    CFRelease(s);
    CHECK(deallocs > 0);
    CFRelease(a);

    CFDictionaryKeyCallBacks kcb = {0, NULL, NULL, NULL, key_equal, key_hash};
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kcb, &kCFTypeDictionaryValueCallBacks);
    my_key k1 = {42}, k2 = {42}, k3 = {7};
    CFDictionarySetValue(d, &k1, CFSTR("answer"));
    CHECK(CFDictionaryGetValue(d, &k2) == CFSTR("answer"));
    CHECK(CFDictionaryGetValue(d, &k3) == NULL);
    CFRelease(d);

    // CFString with a guest no-copy buffer.
    char *heap = strdup("no copy string");
    CFStringRef nc = CFStringCreateWithCStringNoCopy(NULL, heap, kCFStringEncodingASCII, kCFAllocatorMalloc);
    CHECK(strcmp(cstr(nc), "no copy string") == 0);
    CFRelease(nc);
    CFDataRef nd = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)"static bytes", 12, kCFAllocatorNull);
    CHECK(CFDataGetBytePtr(nd) && memcmp(CFDataGetBytePtr(nd), "static bytes", 12) == 0);
    CFRelease(nd);

    // Property list round trip.
    CFMutableDictionaryRef pl =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(pl, CFSTR("k"), CFSTR("v"));
    CFDataRef x = CFPropertyListCreateData(NULL, pl, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFPropertyListFormat fmt = 0;
    CFPropertyListRef back = CFPropertyListCreateWithData(NULL, x, kCFPropertyListImmutable, &fmt, NULL);
    CHECK(back && CFEqual(back, pl) && fmt == kCFPropertyListXMLFormat_v1_0);
    CFStringRef err = NULL;
    CFPropertyListRef back2 = CFPropertyListCreateFromXMLData(NULL, x, kCFPropertyListImmutable, &err);
    CHECK(back2 && !err && CFEqual(back2, pl));
    CFRelease(back2);
    CFRelease(back);
    CFRelease(x);
    CFRelease(pl);
}

int main(void) {
    test_runloop();
    test_bundle();
    test_prefs();
    test_url();
    test_dates();
    test_xml();
    test_custom();
    printf("cf_runtime: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
