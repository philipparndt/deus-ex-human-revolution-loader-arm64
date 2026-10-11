// CoreFoundation basics: constant CFSTR literals, strings, numbers, arrays,
// dictionaries, sets, data, formatting.
#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(CFIndex) == 4, "i386 CFIndex");
_Static_assert(sizeof(CFRange) == 8, "i386 CFRange");
_Static_assert(sizeof(CFArrayCallBacks) == 20, "i386 CFArrayCallBacks");
_Static_assert(sizeof(CFDictionaryKeyCallBacks) == 24, "i386 key callbacks");
_Static_assert(sizeof(CFRunLoopTimerContext) == 20, "i386 timer context");
_Static_assert(sizeof(CFGregorianDate) == 16, "i386 CFGregorianDate");
_Static_assert(offsetof(CFGregorianDate, second) == 8, "i386 CFGregorianDate.second");
_Static_assert(sizeof(CFAllocatorContext) == 36, "i386 CFAllocatorContext");

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static char buf[256];
static const char *cstr(CFStringRef s) {
    if (!s) return "(null)";
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return "(conversion failed)";
    return buf;
}

static void applier(const void *k, const void *v, void *ctx) { (*(int *)ctx)++; }
static void set_applier(const void *v, void *ctx) { *(intptr_t *)ctx += (intptr_t)v; }
static CFComparisonResult cmp_ints(const void *a, const void *b, void *ctx) {
    return (intptr_t)a < (intptr_t)b ? kCFCompareLessThan : (intptr_t)a > (intptr_t)b ? kCFCompareGreaterThan : kCFCompareEqualTo;
}

int main(void) {
    // Constant strings.
    CFStringRef lit = CFSTR("Deus Ex");
    CHECK(CFGetTypeID(lit) == CFStringGetTypeID());
    CHECK(CFStringGetLength(lit) == 7);
    CHECK(strcmp(cstr(lit), "Deus Ex") == 0);
    const char *p = CFStringGetCStringPtr(lit, kCFStringEncodingMacRoman);
    CHECK(p && strcmp(p, "Deus Ex") == 0);
    CFStringRef uni = CFSTR("Café – über");
    CHECK(CFStringGetLength(uni) == 11);
    CHECK(CFStringGetCharacterAtIndex(uni, 3) == 0xe9);
    printf("unicode literal: %s\n", cstr(uni));
    CFRetain(lit);
    CFRelease(lit);
    CFRelease(lit); // literals are immortal

    // Created strings.
    CFStringRef s = CFStringCreateWithCString(NULL, "Human Revolution", kCFStringEncodingUTF8);
    CHECK(CFStringGetLength(s) == 16);
    CHECK(CFStringCompare(s, CFSTR("human revolution"), kCFCompareCaseInsensitive) == kCFCompareEqualTo);
    CHECK(CFStringCompare(s, CFSTR("Zzz"), 0) == kCFCompareLessThan);
    CFRange rg = CFStringFind(s, CFSTR("Revo"), 0);
    CHECK(rg.location == 6 && rg.length == 4);
    CFRange res;
    CHECK(CFStringFindWithOptions(s, CFSTR("ion"), CFRangeMake(0, 16), kCFCompareBackwards, &res));
    CHECK(res.location == 13 && res.length == 3);
    CFMutableStringRef m = CFStringCreateMutableCopy(NULL, 0, s);
    CFStringAppend(m, CFSTR(" DC"));
    CFStringInsert(m, 0, CFSTR(">"));
    CFStringUppercase(m, NULL);
    CHECK(strcmp(cstr(m), ">HUMAN REVOLUTION DC") == 0);
    CFStringRef sub = CFStringCreateWithSubstring(NULL, m, CFRangeMake(1, 5));
    CHECK(strcmp(cstr(sub), "HUMAN") == 0);
    CHECK(CFStringFindAndReplace(m, CFSTR("O"), CFSTR("0"), CFRangeMake(0, CFStringGetLength(m)), 0) == 2);
    CHECK(strcmp(cstr(m), ">HUMAN REV0LUTI0N DC") == 0);
    UniChar chars[4];
    CFStringGetCharacters(m, CFRangeMake(1, 4), chars);
    CHECK(chars[0] == 'H' && chars[3] == 'A');
    UInt8 bytes[16];
    CFIndex used = 0;
    CFIndex got = CFStringGetBytes(m, CFRangeMake(0, 6), kCFStringEncodingASCII, 0, false, bytes, sizeof bytes, &used);
    CHECK(got == 6 && used == 6 && memcmp(bytes, ">HUMAN", 6) == 0);
    CHECK(CFStringGetIntValue(CFSTR("  42")) == 42);
    CHECK(CFStringGetDoubleValue(CFSTR("2.5")) == 2.5);
    CFRelease(sub);
    CFRelease(m);

    CFStringRef f = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %d %5.2f %s %lld %x %C|%-4s|%2$d"), s, 42, 3.14159,
                                             "cstr", 1234567890123LL, 0xbeef, (UniChar)0xe9, "ab");
    printf("format: %s\n", cstr(f));
    CHECK(strcmp(cstr(f), "Human Revolution 42  3.14 cstr 1234567890123 beef \xc3\xa9|ab  |42") == 0);
    CFRelease(f);
    CFStringRef pf = CFStringCreateWithFormat(NULL, NULL, CFSTR("%2$@-%1$@"), CFSTR("a"), CFSTR("b"));
    CHECK(strcmp(cstr(pf), "b-a") == 0);
    CFRelease(pf);

    // Numbers.
    long lv = 123456;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongType, &lv);
    SInt64 v64 = 0;
    CHECK(CFNumberGetValue(n, kCFNumberSInt64Type, &v64) && v64 == 123456);
    CFIndex idx = 0;
    CHECK(CFNumberGetValue(n, kCFNumberCFIndexType, &idx) && idx == 123456);
    CGFloat cgf = 1.5f;
    CFNumberRef nf = CFNumberCreate(NULL, kCFNumberCGFloatType, &cgf);
    double dv = 0;
    CHECK(CFNumberGetValue(nf, kCFNumberDoubleType, &dv) && dv == 1.5);
    CHECK(CFNumberIsFloatType(nf));
    CHECK(CFBooleanGetValue(kCFBooleanTrue) && !CFBooleanGetValue(kCFBooleanFalse));

    // Arrays with CFType callbacks.
    const void *vals[3] = {lit, n, kCFBooleanTrue};
    CFArrayRef arr = CFArrayCreate(NULL, vals, 3, &kCFTypeArrayCallBacks);
    CHECK(CFArrayGetCount(arr) == 3);
    CHECK(CFArrayGetValueAtIndex(arr, 0) == lit); // literal identity preserved
    CHECK(CFArrayGetValueAtIndex(arr, 1) == n);
    CHECK(CFArrayGetValueAtIndex(arr, 2) == kCFBooleanTrue);
    CFMutableArrayRef ma = CFArrayCreateMutableCopy(NULL, 0, arr);
    CFArrayAppendValue(ma, s);
    CFArrayInsertValueAtIndex(ma, 0, CFSTR("first"));
    CHECK(CFArrayGetCount(ma) == 5);
    CHECK(CFEqual(CFArrayGetValueAtIndex(ma, 0), CFSTR("first")));
    CHECK(CFArrayGetValueAtIndex(ma, 4) == s);
    CFRelease(ma);
    CFRelease(arr);

    // Raw (NULL callbacks) arrays.
    CFMutableArrayRef raw = CFArrayCreateMutable(NULL, 0, NULL);
    CFArrayAppendValue(raw, (void *)30);
    CFArrayAppendValue(raw, (void *)10);
    CFArrayAppendValue(raw, (void *)20);
    CFArraySortValues(raw, CFRangeMake(0, 3), cmp_ints, NULL);
    CHECK(CFArrayGetValueAtIndex(raw, 0) == (void *)10 && CFArrayGetValueAtIndex(raw, 2) == (void *)30);
    CHECK(CFArrayGetFirstIndexOfValue(raw, CFRangeMake(0, 3), (void *)20) == 1);
    CFRelease(raw);

    // Dictionaries.
    CFMutableDictionaryRef d =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR("name"), s);
    CFDictionarySetValue(d, CFSTR("count"), n);
    CFStringRef key = CFStringCreateWithCString(NULL, "name", kCFStringEncodingASCII);
    CHECK(CFDictionaryGetValue(d, key) == s);
    CHECK(CFDictionaryGetCount(d) == 2);
    const void *out = NULL;
    CHECK(CFDictionaryGetValueIfPresent(d, CFSTR("count"), &out) && out == n);
    CHECK(!CFDictionaryGetValueIfPresent(d, CFSTR("missing"), &out));
    const void *keys[2], *values[2];
    CFDictionaryGetKeysAndValues(d, keys, values);
    CHECK((keys[0] == CFSTR("name") && values[0] == s) || (keys[1] == CFSTR("name") && values[1] == s));
    int napplied = 0;
    CFDictionaryApplyFunction(d, applier, &napplied);
    CHECK(napplied == 2);
    CFDictionaryRef dc = CFDictionaryCreateCopy(NULL, d);
    CHECK(CFEqual(dc, d));
    CFRelease(dc);
    CFRelease(key);
    CFRelease(d);

    CFMutableDictionaryRef rd = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    CFDictionarySetValue(rd, (void *)1, (void *)100);
    CFDictionarySetValue(rd, (void *)2, (void *)200);
    CHECK(CFDictionaryGetValue(rd, (void *)2) == (void *)200);
    CFRelease(rd);

    // Mixed: string keys, raw values.
    CFMutableDictionaryRef mixed = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
    CFDictionarySetValue(mixed, CFSTR("ptr"), (void *)0x1234);
    CHECK(CFDictionaryGetValue(mixed, CFSTR("ptr")) == (void *)0x1234);
    CFRelease(mixed);

    // Sets.
    const void *svals[3] = {(void *)1, (void *)2, (void *)4};
    CFSetRef set = CFSetCreate(NULL, svals, 3, NULL);
    intptr_t sum = 0;
    CFSetApplyFunction(set, set_applier, &sum);
    CHECK(CFSetGetCount(set) == 3 && sum == 7);
    CFRelease(set);

    // Data.
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)"abcdef", 6);
    CHECK(CFDataGetLength(data) == 6);
    CHECK(memcmp(CFDataGetBytePtr(data), "abcdef", 6) == 0);
    UInt8 db[3];
    CFDataGetBytes(data, CFRangeMake(2, 3), db);
    CHECK(memcmp(db, "cde", 3) == 0);
    CFMutableDataRef md = CFDataCreateMutableCopy(NULL, 0, data);
    CFDataAppendBytes(md, (const UInt8 *)"gh", 2);
    CFDataGetMutableBytePtr(md)[0] = 'A';
    CHECK(CFDataGetLength(md) == 8 && memcmp(CFDataGetBytePtr(md), "Abcdefgh", 8) == 0);
    CFDataRef ext = CFStringCreateExternalRepresentation(NULL, CFSTR("xyz"), kCFStringEncodingUTF8, 0);
    CHECK(CFDataGetLength(ext) == 3 && memcmp(CFDataGetBytePtr(ext), "xyz", 3) == 0);
    CFRelease(ext);
    CFRelease(md);
    CFRelease(data);

    CFStringRef desc = CFCopyDescription(n);
    printf("number description: %s\n", cstr(desc));
    CFRelease(desc);
    CFRelease(n);
    CFRelease(nf);
    CFRelease(s);

    printf("cf_basic: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
