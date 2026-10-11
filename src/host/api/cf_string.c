// CoreFoundation: CFString, CFMutableString, CFAttributedString, CFCharacterSet.
#include "cf_bridge.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define S(i) ARGSTR(i)
#define MS(i) ((CFMutableStringRef)ARGCF(i))

THUNK(CFStringGetTypeID) { RET(CFStringGetTypeID()); }
THUNK(CFStringGetLength) {
    CFStringRef s = S(0);
    RET(s ? (uint32_t)CFStringGetLength(s) : 0);
}

// --- creation -------------------------------------------------------------------------
THUNK(CFStringCreateWithCString) {
    if (!ARG(1)) { RET(0); return; }
    RET_CF(CFStringCreateWithCString(gcf_allocator(ARG(0)), ARGS(1), ARG(2)));
}
THUNK(CFStringCreateWithPascalString) {
    if (!ARG(1)) { RET(0); return; }
    RET_CF(CFStringCreateWithPascalString(gcf_allocator(ARG(0)), ARGP(1), ARG(2)));
}
THUNK(CFStringCreateWithBytes) {
    RET_CF(CFStringCreateWithBytes(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), ARG(3), ARGBOOL(4)));
}
THUNK(CFStringCreateWithCharacters) {
    RET_CF(CFStringCreateWithCharacters(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2)));
}

// NoCopy variants copy the guest bytes and then dispose of the guest buffer
// according to the contents deallocator.
static void dispose_guest_buffer(gaddr_t buf, gaddr_t deallocator) {
    if (!buf) return;
    CFAllocatorRef a = gcf_allocator(deallocator);
    if (a == kCFAllocatorNull) return;
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc || a == kCFAllocatorDefault)
        gheap_free(buf);
    else
        CFAllocatorDeallocate(a, g2h(buf));
}
THUNK(CFStringCreateWithCStringNoCopy) {
    if (!ARG(1)) { RET(0); return; }
    CFStringRef s = CFStringCreateWithCString(gcf_allocator(ARG(0)), ARGS(1), ARG(2));
    dispose_guest_buffer(ARG(1), ARG(3));
    RET_CF(s);
}
THUNK(CFStringCreateWithBytesNoCopy) {
    CFStringRef s = CFStringCreateWithBytes(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), ARG(3), ARGBOOL(4));
    dispose_guest_buffer(ARG(1), ARG(5));
    RET_CF(s);
}
THUNK(CFStringCreateWithCharactersNoCopy) {
    CFStringRef s = CFStringCreateWithCharacters(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2));
    dispose_guest_buffer(ARG(1), ARG(3));
    RET_CF(s);
}
THUNK(CFStringCreateWithFileSystemRepresentation) {
    if (!ARG(1)) { RET(0); return; }
    RET_CF(CFStringCreateWithFileSystemRepresentation(gcf_allocator(ARG(0)), ARGS(1)));
}
THUNK(CFStringCreateCopy) { RET_CF(CFStringCreateCopy(gcf_allocator(ARG(0)), S(1))); }
THUNK(CFStringCreateWithSubstring) { RET_CF(CFStringCreateWithSubstring(gcf_allocator(ARG(0)), S(1), ARGRANGE(2))); }
THUNK(CFStringCreateMutable) { RET_CF(CFStringCreateMutable(gcf_allocator(ARG(0)), ARGIDX(1))); }
THUNK(CFStringCreateMutableCopy) { RET_CF(CFStringCreateMutableCopy(gcf_allocator(ARG(0)), ARGIDX(1), S(2))); }
THUNK(CFStringCreateExternalRepresentation) {
    RET_CF(CFStringCreateExternalRepresentation(gcf_allocator(ARG(0)), S(1), ARG(2), (UInt8)ARG(3)));
}
THUNK(CFStringCreateFromExternalRepresentation) {
    RET_CF(CFStringCreateFromExternalRepresentation(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), ARG(2)));
}
THUNK(CFStringCreateWithFormat) {
    // (alloc, formatOptions, format, ...)
    RET_CF(cf_format_guest(S(2), GVA_FROM_ARG(3)));
}
THUNK(CFStringCreateWithFormatAndArguments) { RET_CF(cf_format_guest(S(2), gva_at(ARG(3)))); }
THUNK(CFStringAppendFormat) {
    CFStringRef t = cf_format_guest(S(2), GVA_FROM_ARG(3));
    if (t) {
        CFStringAppend(MS(0), t);
        CFRelease(t);
    }
}
THUNK(CFStringAppendFormatAndArguments) {
    CFStringRef t = cf_format_guest(S(2), gva_at(ARG(3)));
    if (t) {
        CFStringAppend(MS(0), t);
        CFRelease(t);
    }
}
THUNK(CFStringCreateArrayBySeparatingStrings) {
    RET_CF(CFStringCreateArrayBySeparatingStrings(gcf_allocator(ARG(0)), S(1), S(2)));
}
THUNK(CFStringCreateByCombiningStrings) {
    RET_CF(CFStringCreateByCombiningStrings(gcf_allocator(ARG(0)), (CFArrayRef)ARGCF(1), S(2)));
}

// --- access ----------------------------------------------------------------------------
THUNK(CFStringGetCharacterAtIndex) { RET(CFStringGetCharacterAtIndex(S(0), ARGIDX(1))); }
THUNK(CFStringGetCharacters) { CFStringGetCharacters(S(0), ARGRANGE(1), ARGP(3)); }
THUNK(CFStringGetCString) {
    CFStringRef s = S(0);
    if (!s || !ARG(1)) { RET(0); return; }
    RET_BOOL(CFStringGetCString(s, ARGP(1), ARGIDX(2), ARG(3)));
}
THUNK(CFStringGetPascalString) { RET_BOOL(CFStringGetPascalString(S(0), ARGP(1), ARGIDX(2), ARG(3))); }
// Interior pointers: return the literal's own guest bytes for 8-bit guest
// constants (ASCII-compatible encodings), otherwise NULL (allowed by the API).
THUNK(CFStringGetCStringPtr) {
    gaddr_t g = ARG(0);
    uint32_t enc = ARG(1);
    if (cf_is_guest_constant(g) && !(gread32(g + 4) & 0x10)) {
        // Guest literal chars are NUL-terminated ASCII (non-ASCII literals are UTF-16).
        if (enc == kCFStringEncodingASCII || enc == kCFStringEncodingUTF8 || enc == kCFStringEncodingMacRoman ||
            enc == kCFStringEncodingISOLatin1 || enc == kCFStringEncodingWindowsLatin1 ||
            enc == kCFStringEncodingNonLossyASCII) {
            RET(gread32(g + 8));
            return;
        }
    }
    RET(0);
}
THUNK(CFStringGetCharactersPtr) {
    gaddr_t g = ARG(0);
    if (cf_is_guest_constant(g) && (gread32(g + 4) & 0x10)) { RET(gread32(g + 8)); return; }
    RET(0);
}
THUNK(CFStringGetBytes) {
    // (s, range, enc, lossByte, isExternal, buffer, maxBufLen, usedBufLen*)
    CFIndex used = 0;
    CFIndex n = CFStringGetBytes(S(0), ARGRANGE(1), ARG(3), (UInt8)ARG(4), ARGBOOL(5), ARGP(6), ARGIDX(7), &used);
    if (ARG(8)) gwrite32(ARG(8), (uint32_t)used);
    RET(n);
}
THUNK(CFStringGetIntValue) { RET(CFStringGetIntValue(S(0))); }
THUNK(CFStringGetDoubleValue) { RETD(CFStringGetDoubleValue(S(0))); }
THUNK(CFStringGetFileSystemRepresentation) {
    CFStringRef s = S(0);
    if (!s || !ARG(1)) { RET(0); return; }
    RET_BOOL(CFStringGetFileSystemRepresentation(s, ARGP(1), ARGIDX(2)));
}
THUNK(CFStringGetMaximumSizeOfFileSystemRepresentation) {
    RET((uint32_t)CFStringGetMaximumSizeOfFileSystemRepresentation(S(0)));
}
THUNK(CFStringGetMaximumSizeForEncoding) { RET((uint32_t)CFStringGetMaximumSizeForEncoding(ARGIDX(0), ARG(1))); }
THUNK(CFStringGetSystemEncoding) { RET(CFStringGetSystemEncoding()); }
THUNK(CFStringGetFastestEncoding) { RET(CFStringGetFastestEncoding(S(0))); }
THUNK(CFStringGetSmallestEncoding) { RET(CFStringGetSmallestEncoding(S(0))); }
THUNK(CFStringIsEncodingAvailable) { RET_BOOL(CFStringIsEncodingAvailable(ARG(0))); }
THUNK(CFStringConvertEncodingToNSStringEncoding) { RET((uint32_t)CFStringConvertEncodingToNSStringEncoding(ARG(0))); }
THUNK(CFStringConvertNSStringEncodingToEncoding) { RET(CFStringConvertNSStringEncodingToEncoding(ARG(0))); }
THUNK(CFStringConvertIANACharSetNameToEncoding) { RET(CFStringConvertIANACharSetNameToEncoding(S(0))); }
THUNK(CFStringConvertEncodingToIANACharSetName) { RET_CF(CFStringConvertEncodingToIANACharSetName(ARG(0))); }
THUNK(CFStringConvertWindowsCodepageToEncoding) { RET(CFStringConvertWindowsCodepageToEncoding(ARG(0))); }
THUNK(CFStringConvertEncodingToWindowsCodepage) { RET(CFStringConvertEncodingToWindowsCodepage(ARG(0))); }
THUNK(CFStringGetNameOfEncoding) { RET_CF(CFStringGetNameOfEncoding(ARG(0))); }

// --- comparison / search -------------------------------------------------------------------
THUNK(CFStringCompare) { RET((uint32_t)CFStringCompare(S(0), S(1), ARG(2))); }
THUNK(CFStringCompareWithOptions) { RET((uint32_t)CFStringCompareWithOptions(S(0), S(1), ARGRANGE(2), ARG(4))); }
THUNK(CFStringCompareWithOptionsAndLocale) {
    RET((uint32_t)CFStringCompareWithOptionsAndLocale(S(0), S(1), ARGRANGE(2), ARG(4), (CFLocaleRef)ARGCF(5)));
}
THUNK(CFStringHasPrefix) { RET_BOOL(CFStringHasPrefix(S(0), S(1))); }
THUNK(CFStringHasSuffix) { RET_BOOL(CFStringHasSuffix(S(0), S(1))); }
THUNK(CFStringFind) { RET_RANGE(CFStringFind(S(0), S(1), ARG(2))); }
THUNK(CFStringFindWithOptions) {
    // (s, find, range, options, CFRange *result)
    CFRange res = {kCFNotFound, 0};
    Boolean ok = CFStringFindWithOptions(S(0), S(1), ARGRANGE(2), ARG(4), &res);
    if (ok) put_grange(ARG(5), res);
    RET_BOOL(ok);
}
THUNK(CFStringFindCharacterFromSet) {
    CFRange res = {kCFNotFound, 0};
    Boolean ok = CFStringFindCharacterFromSet(S(0), (CFCharacterSetRef)ARGCF(1), ARGRANGE(2), ARG(4), &res);
    if (ok) put_grange(ARG(5), res);
    RET_BOOL(ok);
}
THUNK(CFStringCreateArrayWithFindResults) {
    // Returns an array of CFRange* values (host pointers): convert to a guest
    // array of guest CFRange structs is not representable; rebuild as raw.
    CFArrayRef a = CFStringCreateArrayWithFindResults(gcf_allocator(ARG(0)), S(1), S(2), ARGRANGE(3), ARG(5));
    if (!a) { RET(0); return; }
    CFIndex n = CFArrayGetCount(a);
    CFMutableArrayRef out = CFArrayCreateMutable(NULL, n, NULL);
    for (CFIndex i = 0; i < n; i++) {
        const CFRange *rg = CFArrayGetValueAtIndex(a, i);
        gaddr_t g = gheap_alloc(8);
        put_grange(g, *rg);
        CFArrayAppendValue(out, (const void *)(uintptr_t)g);
    }
    CFRelease(a);
    cf_set_raw_flags(out, CF_RAW_VALUES);
    RET_CF(out);
}

// --- mutation ---------------------------------------------------------------------------------
THUNK(CFStringAppend) { CFStringAppend(MS(0), S(1)); }
THUNK(CFStringAppendCString) { CFStringAppendCString(MS(0), ARGS(1), ARG(2)); }
THUNK(CFStringAppendCharacters) { CFStringAppendCharacters(MS(0), ARGP(1), ARGIDX(2)); }
THUNK(CFStringAppendPascalString) { CFStringAppendPascalString(MS(0), ARGP(1), ARG(2)); }
THUNK(CFStringInsert) { CFStringInsert(MS(0), ARGIDX(1), S(2)); }
THUNK(CFStringDelete) { CFStringDelete(MS(0), ARGRANGE(1)); }
THUNK(CFStringReplace) { CFStringReplace(MS(0), ARGRANGE(1), S(3)); }
THUNK(CFStringReplaceAll) { CFStringReplaceAll(MS(0), S(1)); }
THUNK(CFStringFindAndReplace) {
    // (s, find, replace, range, options)
    RET((uint32_t)CFStringFindAndReplace(MS(0), S(1), S(2), ARGRANGE(3), ARG(5)));
}
THUNK(CFStringSetExternalCharactersNoCopy) {
    fatal("CFStringSetExternalCharactersNoCopy is not supported (guest buffer ownership)");
}
THUNK(CFStringPad) { CFStringPad(MS(0), S(1), ARGIDX(2), ARGIDX(3)); }
THUNK(CFStringTrim) { CFStringTrim(MS(0), S(1)); }
THUNK(CFStringTrimWhitespace) { CFStringTrimWhitespace(MS(0)); }
THUNK(CFStringLowercase) { CFStringLowercase(MS(0), (CFLocaleRef)ARGCF(1)); }
THUNK(CFStringUppercase) { CFStringUppercase(MS(0), (CFLocaleRef)ARGCF(1)); }
THUNK(CFStringCapitalize) { CFStringCapitalize(MS(0), (CFLocaleRef)ARGCF(1)); }
THUNK(CFStringNormalize) { CFStringNormalize(MS(0), ARG(1)); }
THUNK(CFStringFold) { CFStringFold(MS(0), ARG(1), (CFLocaleRef)ARGCF(2)); }
THUNK(CFStringTransform) {
    CFRange rg, *prg = NULL;
    if (ARG(1)) {
        rg = CFRangeMake((int32_t)gread32(ARG(1)), (int32_t)gread32(ARG(1) + 4));
        prg = &rg;
    }
    Boolean ok = CFStringTransform(MS(0), prg, S(2), ARGBOOL(3));
    if (prg) put_grange(ARG(1), rg);
    RET_BOOL(ok);
}

// --- CFAttributedString ---------------------------------------------------------------------
THUNK(CFAttributedStringGetTypeID) { RET(CFAttributedStringGetTypeID()); }
THUNK(CFAttributedStringCreate) {
    RET_CF(CFAttributedStringCreate(gcf_allocator(ARG(0)), S(1), (CFDictionaryRef)ARGCF(2)));
}
THUNK(CFAttributedStringCreateMutable) { RET_CF(CFAttributedStringCreateMutable(gcf_allocator(ARG(0)), ARGIDX(1))); }
THUNK(CFAttributedStringGetString) { RET_CF(CFAttributedStringGetString((CFAttributedStringRef)ARGCF(0))); }
THUNK(CFAttributedStringGetLength) { RET((uint32_t)CFAttributedStringGetLength((CFAttributedStringRef)ARGCF(0))); }
THUNK(CFAttributedStringReplaceString) {
    CFAttributedStringReplaceString((CFMutableAttributedStringRef)ARGCF(0), ARGRANGE(1), S(3));
}
THUNK(CFAttributedStringSetAttribute) {
    CFAttributedStringSetAttribute((CFMutableAttributedStringRef)ARGCF(0), ARGRANGE(1), S(3), ARGCF(4));
}
THUNK(CFAttributedStringSetAttributes) {
    CFAttributedStringSetAttributes((CFMutableAttributedStringRef)ARGCF(0), ARGRANGE(1), (CFDictionaryRef)ARGCF(3),
                                    ARGBOOL(4));
}

// --- CFCharacterSet -------------------------------------------------------------------------------
THUNK(CFCharacterSetGetPredefined) { RET_CF(CFCharacterSetGetPredefined(ARGIDX(0))); }
THUNK(CFCharacterSetIsCharacterMember) { RET_BOOL(CFCharacterSetIsCharacterMember((CFCharacterSetRef)ARGCF(0), (UniChar)ARG(1))); }
THUNK(CFCharacterSetCreateWithCharactersInString) {
    RET_CF(CFCharacterSetCreateWithCharactersInString(gcf_allocator(ARG(0)), S(1)));
}
