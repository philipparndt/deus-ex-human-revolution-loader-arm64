// CoreFoundation: CFBundle, CFPreferences, CFPropertyList, CFURL.
#include "cf_bridge.h"

#include "../macho.h"
#include "../runtime.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define CF_CONST(sym)                                                                              \
    static void init_cfc_##sym(gaddr_t a) { gwrite32(a, cfg((CFTypeRef)(sym))); }                \
    GDATA(sym, 4, init_cfc_##sym)

#define B(i) ((CFBundleRef)ARGCF(i))
#define U(i) ((CFURLRef)ARGCF(i))

// The guest's view of "the main bundle" is the game's .app.
static CFBundleRef gbundle(gaddr_t g) {
    CFBundleRef b = (CFBundleRef)gcf(g);
    if (b && b == CFBundleGetMainBundle()) return cf_main_bundle();
    return b;
}
#define GB(i) gbundle(ARG(i))

// --- CFBundle ---------------------------------------------------------------------------
CF_CONST(kCFBundleIdentifierKey);
CF_CONST(kCFBundleNameKey);
CF_CONST(kCFBundleVersionKey);
CF_CONST(kCFBundleExecutableKey);
CF_CONST(kCFBundleInfoDictionaryVersionKey);
CF_CONST(kCFBundleLocalizationsKey);
CF_CONST(kCFBundleDevelopmentRegionKey);

THUNK(CFBundleGetTypeID) { RET(CFBundleGetTypeID()); }
THUNK(CFBundleGetMainBundle) { RET_CF(cf_main_bundle()); }
THUNK(CFBundleCreate) {
    CFURLRef url = U(1);
    CFBundleRef b = url ? CFBundleCreate(gcf_allocator(ARG(0)), url) : NULL;
    RET_CF(b);
}
THUNK(CFBundleGetBundleWithIdentifier) {
    CFStringRef id = ARGSTR(0);
    CFBundleRef mb = cf_main_bundle();
    CFStringRef mid = CFBundleGetIdentifier(mb);
    if (id && mid && CFEqual(id, mid)) {
        RET_CF(mb);
        return;
    }
    RET_CF(id ? CFBundleGetBundleWithIdentifier(id) : NULL);
}
THUNK(CFBundleGetIdentifier) { RET_CF(CFBundleGetIdentifier(GB(0))); }
THUNK(CFBundleGetInfoDictionary) { RET_CF(CFBundleGetInfoDictionary(GB(0))); }
THUNK(CFBundleGetLocalInfoDictionary) { RET_CF(CFBundleGetLocalInfoDictionary(GB(0))); }
THUNK(CFBundleGetValueForInfoDictionaryKey) { RET_CF(CFBundleGetValueForInfoDictionaryKey(GB(0), ARGSTR(1))); }
THUNK(CFBundleGetVersionNumber) { RET(CFBundleGetVersionNumber(GB(0))); }
THUNK(CFBundleGetDevelopmentRegion) { RET_CF(CFBundleGetDevelopmentRegion(GB(0))); }
THUNK(CFBundleGetPackageInfo) {
    UInt32 type = 0, creator = 0;
    CFBundleGetPackageInfo(GB(0), &type, &creator);
    if (ARG(1)) gwrite32(ARG(1), type);
    if (ARG(2)) gwrite32(ARG(2), creator);
}
THUNK(CFBundleCopyBundleURL) { RET_CF(CFBundleCopyBundleURL(GB(0))); }
THUNK(CFBundleCopyExecutableURL) { RET_CF(CFBundleCopyExecutableURL(GB(0))); }
THUNK(CFBundleCopyAuxiliaryExecutableURL) { RET_CF(CFBundleCopyAuxiliaryExecutableURL(GB(0), ARGSTR(1))); }
THUNK(CFBundleCopyResourcesDirectoryURL) { RET_CF(CFBundleCopyResourcesDirectoryURL(GB(0))); }
THUNK(CFBundleCopyPrivateFrameworksURL) { RET_CF(CFBundleCopyPrivateFrameworksURL(GB(0))); }
THUNK(CFBundleCopySharedFrameworksURL) { RET_CF(CFBundleCopySharedFrameworksURL(GB(0))); }
THUNK(CFBundleCopySharedSupportURL) { RET_CF(CFBundleCopySharedSupportURL(GB(0))); }
THUNK(CFBundleCopyBuiltInPlugInsURL) { RET_CF(CFBundleCopyBuiltInPlugInsURL(GB(0))); }
THUNK(CFBundleCopyResourceURL) { RET_CF(CFBundleCopyResourceURL(GB(0), ARGSTR(1), ARGSTR(2), ARGSTR(3))); }
THUNK(CFBundleCopyResourceURLsOfType) { RET_CF(CFBundleCopyResourceURLsOfType(GB(0), ARGSTR(1), ARGSTR(2))); }
THUNK(CFBundleCopyResourceURLForLocalization) {
    RET_CF(CFBundleCopyResourceURLForLocalization(GB(0), ARGSTR(1), ARGSTR(2), ARGSTR(3), ARGSTR(4)));
}
THUNK(CFBundleCopyLocalizedString) {
    RET_CF(CFBundleCopyLocalizedString(GB(0), ARGSTR(1), ARGSTR(2), ARGSTR(3)));
}
THUNK(CFBundleCopyBundleLocalizations) { RET_CF(CFBundleCopyBundleLocalizations(GB(0))); }
THUNK(CFBundleCopyPreferredLocalizationsFromArray) {
    RET_CF(CFBundleCopyPreferredLocalizationsFromArray((CFArrayRef)ARGCF(0)));
}
THUNK(CFBundleCopyLocalizationsForPreferences) {
    RET_CF(CFBundleCopyLocalizationsForPreferences((CFArrayRef)ARGCF(0), (CFArrayRef)ARGCF(1)));
}
THUNK(CFBundleCopyInfoDictionaryInDirectory) { RET_CF(CFBundleCopyInfoDictionaryInDirectory(U(0))); }
THUNK(CFBundleCopyInfoDictionaryForURL) { RET_CF(CFBundleCopyInfoDictionaryForURL(U(0))); }
// Executables of guest bundles are guest images loaded by our loader; others
// (system frameworks) are represented by host thunks.
THUNK(CFBundleIsExecutableLoaded) { RET(1); }
THUNK(CFBundleLoadExecutable) {
    LOGD("CFBundleLoadExecutable(%p): pretending loaded", GB(0));
    RET(1);
}
THUNK(CFBundleLoadExecutableAndReturnError) {
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET(1);
}
THUNK(CFBundlePreflightExecutable) {
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET(1);
}
THUNK(CFBundleUnloadExecutable) {}

static gaddr_t lookup_guest_symbol(CFStringRef name) {
    char *n = cf_utf8(name);
    if (!n) return 0;
    char sym[512];
    snprintf(sym, sizeof sym, "_%s", n);
    gaddr_t a = 0;
    for (int i = 0; i < runtime_image_count() && !a; i++) a = macho_export(runtime_image(i), sym, NULL);
    if (!a) a = hostsym_lookup(sym);
    if (!a) LOGI("CFBundleGetFunctionPointerForName: %s not available", n);
    else LOGD("CFBundleGetFunctionPointerForName: %s -> %#x", n, a);
    free(n);
    return a;
}
THUNK(CFBundleGetFunctionPointerForName) { RET(lookup_guest_symbol(ARGSTR(1))); }
THUNK(CFBundleGetDataPointerForName) { RET(lookup_guest_symbol(ARGSTR(1))); }
THUNK(CFBundleGetFunctionPointersForNames) {
    CFArrayRef names = (CFArrayRef)ARGCF(1);
    CFIndex n = names ? CFArrayGetCount(names) : 0;
    for (CFIndex i = 0; i < n; i++)
        gwrite32(ARG(2) + 4 * (uint32_t)i, lookup_guest_symbol(CFArrayGetValueAtIndex(names, i)));
}

// --- CFPreferences ------------------------------------------------------------------------------
CF_CONST(kCFPreferencesAnyApplication);
CF_CONST(kCFPreferencesCurrentApplication);
CF_CONST(kCFPreferencesAnyHost);
CF_CONST(kCFPreferencesCurrentHost);
CF_CONST(kCFPreferencesAnyUser);
CF_CONST(kCFPreferencesCurrentUser);

// kCFPreferencesCurrentApplication means the game, not dxhost.
static CFStringRef pref_app(gaddr_t g) {
    CFStringRef app = gcf_string(g);
    if (app && CFEqual(app, kCFPreferencesCurrentApplication)) {
        CFStringRef id = CFBundleGetIdentifier(cf_main_bundle());
        if (id) return id;
    }
    return app;
}

THUNK(CFPreferencesCopyAppValue) { RET_CF(CFPreferencesCopyAppValue(ARGSTR(0), pref_app(ARG(1)))); }
THUNK(CFPreferencesCopyValue) {
    RET_CF(CFPreferencesCopyValue(ARGSTR(0), pref_app(ARG(1)), ARGSTR(2), ARGSTR(3)));
}
THUNK(CFPreferencesGetAppBooleanValue) {
    Boolean valid = false;
    Boolean v = CFPreferencesGetAppBooleanValue(ARGSTR(0), pref_app(ARG(1)), &valid);
    if (ARG(2)) gwrite8(ARG(2), valid);
    RET_BOOL(v);
}
THUNK(CFPreferencesGetAppIntegerValue) {
    Boolean valid = false;
    CFIndex v = CFPreferencesGetAppIntegerValue(ARGSTR(0), pref_app(ARG(1)), &valid);
    if (ARG(2)) gwrite8(ARG(2), valid);
    RET((uint32_t)v);
}
THUNK(CFPreferencesSetAppValue) { CFPreferencesSetAppValue(ARGSTR(0), ARGCF(1), pref_app(ARG(2))); }
THUNK(CFPreferencesSetValue) {
    CFPreferencesSetValue(ARGSTR(0), ARGCF(1), pref_app(ARG(2)), ARGSTR(3), ARGSTR(4));
}
THUNK(CFPreferencesAppSynchronize) { RET_BOOL(CFPreferencesAppSynchronize(pref_app(ARG(0)))); }
THUNK(CFPreferencesSynchronize) { RET_BOOL(CFPreferencesSynchronize(pref_app(ARG(0)), ARGSTR(1), ARGSTR(2))); }
THUNK(CFPreferencesAppValueIsForced) { RET_BOOL(CFPreferencesAppValueIsForced(ARGSTR(0), pref_app(ARG(1)))); }
THUNK(CFPreferencesCopyKeyList) { RET_CF(CFPreferencesCopyKeyList(pref_app(ARG(0)), ARGSTR(1), ARGSTR(2))); }
THUNK(CFPreferencesCopyMultiple) {
    RET_CF(CFPreferencesCopyMultiple((CFArrayRef)ARGCF(0), pref_app(ARG(1)), ARGSTR(2), ARGSTR(3)));
}
THUNK(CFPreferencesSetMultiple) {
    CFPreferencesSetMultiple((CFDictionaryRef)ARGCF(0), (CFArrayRef)ARGCF(1), pref_app(ARG(2)), ARGSTR(3), ARGSTR(4));
}
THUNK(CFPreferencesAddSuitePreferencesToApp) { CFPreferencesAddSuitePreferencesToApp(pref_app(ARG(0)), ARGSTR(1)); }

// --- CFPropertyList ---------------------------------------------------------------------------------
static void put_err(gaddr_t g, CFTypeRef e) {
    if (g) gwrite32(g, cfg(e));
    else if (e) CFRelease(e);
}
THUNK(CFPropertyListCreateFromXMLData) {
    CFStringRef err = NULL;
    CFPropertyListRef p = CFPropertyListCreateFromXMLData(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), ARG(2),
                                                          ARG(3) ? &err : NULL);
    put_err(ARG(3), err);
    RET_CF(p);
}
THUNK(CFPropertyListCreateWithData) {
    CFPropertyListFormat fmt = 0;
    CFErrorRef err = NULL;
    CFPropertyListRef p =
        CFPropertyListCreateWithData(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), ARG(2), &fmt, ARG(4) ? &err : NULL);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)fmt);
    put_err(ARG(4), err);
    RET_CF(p);
}
THUNK(CFPropertyListCreateXMLData) { RET_CF(CFPropertyListCreateXMLData(gcf_allocator(ARG(0)), ARGCF(1))); }
THUNK(CFPropertyListCreateData) {
    CFErrorRef err = NULL;
    CFDataRef d = CFPropertyListCreateData(gcf_allocator(ARG(0)), ARGCF(1), ARG(2), ARG(3), ARG(4) ? &err : NULL);
    put_err(ARG(4), err);
    RET_CF(d);
}
THUNK(CFPropertyListCreateDeepCopy) { RET_CF(CFPropertyListCreateDeepCopy(gcf_allocator(ARG(0)), ARGCF(1), ARG(2))); }
THUNK(CFPropertyListIsValid) { RET_BOOL(CFPropertyListIsValid(ARGCF(0), ARG(1))); }

// --- CFURL -------------------------------------------------------------------------------------------
THUNK(CFURLGetTypeID) { RET(CFURLGetTypeID()); }
THUNK(CFURLCreateWithString) { RET_CF(CFURLCreateWithString(gcf_allocator(ARG(0)), ARGSTR(1), U(2))); }
THUNK(CFURLCreateWithBytes) {
    RET_CF(CFURLCreateWithBytes(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), ARG(3), U(4)));
}
THUNK(CFURLCreateWithFileSystemPath) {
    CFStringRef p = ARGSTR(1);
    RET_CF(p ? CFURLCreateWithFileSystemPath(gcf_allocator(ARG(0)), p, ARGIDX(2), ARGBOOL(3)) : NULL);
}
THUNK(CFURLCreateWithFileSystemPathRelativeToBase) {
    RET_CF(CFURLCreateWithFileSystemPathRelativeToBase(gcf_allocator(ARG(0)), ARGSTR(1), ARGIDX(2), ARGBOOL(3), U(4)));
}
THUNK(CFURLCreateFromFileSystemRepresentation) {
    if (!ARG(1)) {
        RET(0);
        return;
    }
    RET_CF(CFURLCreateFromFileSystemRepresentation(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), ARGBOOL(3)));
}
THUNK(CFURLCreateFromFileSystemRepresentationRelativeToBase) {
    RET_CF(CFURLCreateFromFileSystemRepresentationRelativeToBase(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), ARGBOOL(3),
                                                                 U(4)));
}
THUNK(CFURLGetFileSystemRepresentation) {
    CFURLRef u = U(0);
    if (!u || !ARG(2)) {
        RET(0);
        return;
    }
    RET_BOOL(CFURLGetFileSystemRepresentation(u, ARGBOOL(1), ARGP(2), ARGIDX(3)));
}
// FSRef is 80 opaque bytes with the same layout on i386 and x86_64.
THUNK(CFURLGetFSRef) {
    CFURLRef u = U(0);
    RET_BOOL(u && ARG(1) && CFURLGetFSRef(u, ARGP(1)));
}
THUNK(CFURLCreateFromFSRef) { RET_CF(ARG(1) ? CFURLCreateFromFSRef(gcf_allocator(ARG(0)), ARGP(1)) : NULL); }
THUNK(CFURLGetString) { RET_CF(CFURLGetString(U(0))); }
THUNK(CFURLGetBaseURL) { RET_CF(CFURLGetBaseURL(U(0))); }
THUNK(CFURLCopyAbsoluteURL) { RET_CF(CFURLCopyAbsoluteURL(U(0))); }
THUNK(CFURLCopyFileSystemPath) { RET_CF(CFURLCopyFileSystemPath(U(0), ARGIDX(1))); }
THUNK(CFURLCopyHostName) { RET_CF(CFURLCopyHostName(U(0))); }
THUNK(CFURLCopyScheme) { RET_CF(CFURLCopyScheme(U(0))); }
THUNK(CFURLCopyPath) { RET_CF(CFURLCopyPath(U(0))); }
THUNK(CFURLCopyQueryString) { RET_CF(CFURLCopyQueryString(U(0), ARGSTR(1))); }
THUNK(CFURLCopyLastPathComponent) { RET_CF(CFURLCopyLastPathComponent(U(0))); }
THUNK(CFURLCopyPathExtension) { RET_CF(CFURLCopyPathExtension(U(0))); }
THUNK(CFURLGetPortNumber) { RET((uint32_t)CFURLGetPortNumber(U(0))); }
THUNK(CFURLHasDirectoryPath) { RET_BOOL(CFURLHasDirectoryPath(U(0))); }
THUNK(CFURLCreateCopyAppendingPathComponent) {
    RET_CF(CFURLCreateCopyAppendingPathComponent(gcf_allocator(ARG(0)), U(1), ARGSTR(2), ARGBOOL(3)));
}
THUNK(CFURLCreateCopyAppendingPathExtension) {
    RET_CF(CFURLCreateCopyAppendingPathExtension(gcf_allocator(ARG(0)), U(1), ARGSTR(2)));
}
THUNK(CFURLCreateCopyDeletingLastPathComponent) {
    RET_CF(CFURLCreateCopyDeletingLastPathComponent(gcf_allocator(ARG(0)), U(1)));
}
THUNK(CFURLCreateCopyDeletingPathExtension) { RET_CF(CFURLCreateCopyDeletingPathExtension(gcf_allocator(ARG(0)), U(1))); }
THUNK(CFURLCreateData) { RET_CF(CFURLCreateData(gcf_allocator(ARG(0)), U(1), ARG(2), ARGBOOL(3))); }
THUNK(CFURLCreateStringByAddingPercentEscapes) {
    RET_CF(CFURLCreateStringByAddingPercentEscapes(gcf_allocator(ARG(0)), ARGSTR(1), ARGSTR(2), ARGSTR(3), ARG(4)));
}
THUNK(CFURLCreateStringByReplacingPercentEscapes) {
    RET_CF(CFURLCreateStringByReplacingPercentEscapes(gcf_allocator(ARG(0)), ARGSTR(1), ARGSTR(2)));
}
THUNK(CFURLCreateStringByReplacingPercentEscapesUsingEncoding) {
    RET_CF(CFURLCreateStringByReplacingPercentEscapesUsingEncoding(gcf_allocator(ARG(0)), ARGSTR(1), ARGSTR(2), ARG(3)));
}
THUNK(CFURLResourceIsReachable) {
    CFErrorRef err = NULL;
    Boolean ok = CFURLResourceIsReachable(U(0), ARG(1) ? &err : NULL);
    put_err(ARG(1), err);
    RET_BOOL(ok);
}
// Bookmarks.
THUNK(CFURLCreateBookmarkData) {
    // (alloc, url, options, resourcePropertiesToInclude, relativeToURL, CFErrorRef *)
    CFErrorRef err = NULL;
    CFDataRef d = CFURLCreateBookmarkData(gcf_allocator(ARG(0)), U(1), ARG(2), (CFArrayRef)ARGCF(3), U(4),
                                          ARG(5) ? &err : NULL);
    put_err(ARG(5), err);
    RET_CF(d);
}
THUNK(CFURLCreateBookmarkDataFromFile) {
    CFErrorRef err = NULL;
    CFDataRef d = CFURLCreateBookmarkDataFromFile(gcf_allocator(ARG(0)), U(1), ARG(2) ? &err : NULL);
    put_err(ARG(2), err);
    RET_CF(d);
}
THUNK(CFURLCreateByResolvingBookmarkData) {
    // (alloc, data, options, relativeToURL, resourcePropertiesToInclude, Boolean *isStale, CFErrorRef *)
    Boolean stale = false;
    CFErrorRef err = NULL;
    CFURLRef u = CFURLCreateByResolvingBookmarkData(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1), ARG(2), U(3),
                                                    (CFArrayRef)ARGCF(4), &stale, ARG(6) ? &err : NULL);
    if (ARG(5)) gwrite8(ARG(5), stale);
    put_err(ARG(6), err);
    RET_CF(u);
}
THUNK(CFURLWriteBookmarkDataToFile) {
    CFErrorRef err = NULL;
    Boolean ok = CFURLWriteBookmarkDataToFile((CFDataRef)ARGCF(0), U(1), ARG(2), ARG(3) ? &err : NULL);
    put_err(ARG(3), err);
    RET_BOOL(ok);
}
THUNK(CFURLCreateBookmarkDataFromAliasRecord) {
    RET_CF(CFURLCreateBookmarkDataFromAliasRecord(gcf_allocator(ARG(0)), (CFDataRef)ARGCF(1)));
}
THUNK(CFURLCreateResourcePropertiesForKeysFromBookmarkData) {
    RET_CF(CFURLCreateResourcePropertiesForKeysFromBookmarkData(gcf_allocator(ARG(0)), (CFArrayRef)ARGCF(1),
                                                                 (CFDataRef)ARGCF(2)));
}
// Legacy resource access (FMOD).
THUNK(CFURLCreateDataAndPropertiesFromResource) {
    // (alloc, url, CFDataRef *data, CFDictionaryRef *props, CFArrayRef desiredProps, SInt32 *errorCode)
    CFDataRef data = NULL;
    CFDictionaryRef props = NULL;
    SInt32 code = 0;
    Boolean ok = CFURLCreateDataAndPropertiesFromResource(gcf_allocator(ARG(0)), U(1), ARG(2) ? &data : NULL,
                                                          ARG(3) ? &props : NULL, (CFArrayRef)ARGCF(4), &code);
    if (ARG(2)) {
        // Hand the guest a data object with guest-visible bytes.
        CFDataRef gd = data ? CFDataCreate(cf_guest_heap_allocator(), CFDataGetBytePtr(data), CFDataGetLength(data)) : NULL;
        if (data) CFRelease(data);
        gwrite32(ARG(2), cfg(gd));
    }
    if (ARG(3)) gwrite32(ARG(3), cfg(props));
    if (ARG(5)) gwrite32(ARG(5), (uint32_t)code);
    RET_BOOL(ok);
}
THUNK(CFURLWriteDataAndPropertiesToResource) {
    SInt32 code = 0;
    Boolean ok = CFURLWriteDataAndPropertiesToResource(U(0), (CFDataRef)ARGCF(1), (CFDictionaryRef)ARGCF(2), &code);
    if (ARG(3)) gwrite32(ARG(3), (uint32_t)code);
    RET_BOOL(ok);
}
THUNK(CFURLDestroyResource) {
    SInt32 code = 0;
    Boolean ok = CFURLDestroyResource(U(0), &code);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)code);
    RET_BOOL(ok);
}
THUNK(CFURLCopyResourcePropertyForKey) {
    CFTypeRef v = NULL;
    CFErrorRef err = NULL;
    Boolean ok = CFURLCopyResourcePropertyForKey(U(0), ARGSTR(1), &v, ARG(3) ? &err : NULL);
    if (ARG(2)) gwrite32(ARG(2), cfg(v));
    put_err(ARG(3), err);
    RET_BOOL(ok);
}
