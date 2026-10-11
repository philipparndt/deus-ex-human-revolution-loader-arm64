// Security.framework subset.
//
//  - Authorization*: nothing privileged is ever run. Rights requests are
//    reported as cancelled, AuthorizationExecuteWithPrivileges is denied.
//  - SecCode*/SecRequirement*: the guest's code-signing checks cannot apply to
//    dxhost; they succeed with placeholder objects.
//  - SecKeychain* generic passwords: a private file-backed store at
//    ~/Library/Application Support/dxhost/keychain.plist (no keychain prompts).
//    Item refs are CFStrings "service\x1F account" (released with CFRelease).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <pthread.h>
#include <sys/stat.h>
#include "ret_return.h"

#define errSecSuccess 0
#define errSecDuplicateItem (-25299)
#define errSecItemNotFound (-25300)
#define errSecParam (-50)
#define errAuthorizationSuccess 0
#define errAuthorizationDenied (-60005)
#define errAuthorizationCanceled (-60006)

// --- Authorization ------------------------------------------------------------------------

THUNK(AuthorizationCreate) {
    LOGI("security: AuthorizationCreate(rights %#x, flags %#x) -> placeholder", ARG(0), ARG(2));
    if (ARG(3)) gwrite32(ARG(3), gheap_calloc(1, 16));
    RET(errAuthorizationSuccess);
}
THUNK(AuthorizationCopyRights) {
    LOGI("security: AuthorizationCopyRights -> canceled (no privileges under dxhost)");
    if (ARG(4)) gwrite32(ARG(4), 0);
    RET(errAuthorizationCanceled);
}
THUNK(AuthorizationExecuteWithPrivileges) {
    LOGE("security: AuthorizationExecuteWithPrivileges(\"%s\") denied", ARG(1) ? ARGS(1) : "");
    if (ARG(4)) gwrite32(ARG(4), 0);
    RET(errAuthorizationDenied);
}
THUNK(AuthorizationFree) {
    if (ARG(0) && gheap_size(ARG(0))) gheap_free(ARG(0));
    RET(errAuthorizationSuccess);
}

// --- code signing ---------------------------------------------------------------------------

static gaddr_t placeholder(const char *what) {
    CFStringRef s = CFStringCreateWithCString(NULL, what, kCFStringEncodingUTF8);
    return cfg(s);
}
THUNK(SecCodeCopySelf) {
    LOGI("security: SecCodeCopySelf -> placeholder");
    gwrite32(ARG(1), placeholder("dxhost-guest-code"));
    RET(errSecSuccess);
}
THUNK(SecCodeCheckValidity) {
    LOGI("security: SecCodeCheckValidity(%#x, flags %#x, req %#x) -> success", ARG(0), ARG(1), ARG(2));
    RET(errSecSuccess);
}
THUNK(SecStaticCodeCheckValidity) { RET(errSecSuccess); }
THUNK(SecRequirementCreateWithString) {
    char *t = cf_utf8(ARGSTR(0));
    LOGI("security: SecRequirementCreateWithString(\"%s\") -> placeholder", t ? t : "");
    free(t);
    gwrite32(ARG(2), placeholder("dxhost-requirement"));
    RET(errSecSuccess);
}
THUNK(SecRequirementCreateWithData) {
    gwrite32(ARG(2), placeholder("dxhost-requirement"));
    RET(errSecSuccess);
}

// --- generic password store -------------------------------------------------------------------

static pthread_mutex_t kc_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef kc;
static char kc_path[1024];

static void kc_load(void) {
    if (kc) return;
    const char *home = getenv("HOME");
    snprintf(kc_path, sizeof kc_path, "%s/Library/Application Support/dxhost", home ? home : "/tmp");
    mkdir(kc_path, 0700);
    strlcat(kc_path, "/keychain.plist", sizeof kc_path);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)kc_path, (CFIndex)strlen(kc_path), false);
    CFReadStreamRef rs = CFReadStreamCreateWithFile(NULL, url);
    CFRelease(url);
    if (rs && CFReadStreamOpen(rs)) {
        CFPropertyListRef p = CFPropertyListCreateWithStream(NULL, rs, 0, kCFPropertyListMutableContainers, NULL, NULL);
        if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) kc = (CFMutableDictionaryRef)p;
        else if (p) CFRelease(p);
        CFReadStreamClose(rs);
    }
    if (rs) CFRelease(rs);
    if (!kc) kc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static void kc_save(void) {
    CFDataRef d = CFPropertyListCreateData(NULL, kc, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
    if (!d) return;
    FILE *f = fopen(kc_path, "wb");
    if (f) {
        fwrite(CFDataGetBytePtr(d), 1, (size_t)CFDataGetLength(d), f);
        fclose(f);
        chmod(kc_path, 0600);
    }
    CFRelease(d);
}

static CFStringRef kc_key(gaddr_t svc, uint32_t svclen, gaddr_t acct, uint32_t acctlen) {
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%.*s\x1F%.*s", (int)svclen, svc ? (const char *)g2h(svc) : "", (int)acctlen,
                     acct ? (const char *)g2h(acct) : "");
    if (n < 0) n = 0;
    CFStringRef k = CFStringCreateWithBytes(NULL, (const UInt8 *)buf, n, kCFStringEncodingUTF8, false);
    return k ? k : CFStringCreateWithBytes(NULL, (const UInt8 *)buf, n, kCFStringEncodingMacRoman, false);
}

// SecKeychainAddGenericPassword(kc, svcLen, svc, acctLen, acct, pwLen, pw, SecKeychainItemRef *item)
THUNK(SecKeychainAddGenericPassword) {
    CFStringRef k = kc_key(ARG(2), ARG(1), ARG(4), ARG(3));
    pthread_mutex_lock(&kc_lock);
    kc_load();
    OSStatus st = errSecSuccess;
    if (CFDictionaryContainsKey(kc, k)) st = errSecDuplicateItem;
    else {
        CFDataRef d = CFDataCreate(NULL, ARGP(6), ARG(5));
        CFDictionarySetValue(kc, k, d);
        CFRelease(d);
        kc_save();
    }
    pthread_mutex_unlock(&kc_lock);
    LOGI("security: add generic password -> %d", (int)st);
    if (ARG(7) && st == errSecSuccess) gwrite32(ARG(7), cfg(k));
    else CFRelease(k);
    RET(st);
}

// SecKeychainFindGenericPassword(kcOrArray, svcLen, svc, acctLen, acct, UInt32 *pwLen, void **pw, item *)
THUNK(SecKeychainFindGenericPassword) {
    CFStringRef k = kc_key(ARG(2), ARG(1), ARG(4), ARG(3));
    pthread_mutex_lock(&kc_lock);
    kc_load();
    CFDataRef d = CFDictionaryGetValue(kc, k);
    OSStatus st = d ? errSecSuccess : errSecItemNotFound;
    if (d) {
        uint32_t n = (uint32_t)CFDataGetLength(d);
        if (ARG(5)) gwrite32(ARG(5), n);
        if (ARG(6)) gwrite32(ARG(6), gheap_memdup(CFDataGetBytePtr(d), n ? n : 1));
    }
    pthread_mutex_unlock(&kc_lock);
    LOGI("security: find generic password -> %d", (int)st);
    if (ARG(7) && d) gwrite32(ARG(7), cfg(k));
    else CFRelease(k);
    RET(st);
}

THUNK(SecKeychainItemFreeContent) {
    if (ARG(1)) gheap_free(ARG(1));
    RET(errSecSuccess);
}

THUNK(SecKeychainItemDelete) {
    CFStringRef k = ARGSTR(0);
    if (!k || CFGetTypeID(k) != CFStringGetTypeID()) RET(errSecParam);
    pthread_mutex_lock(&kc_lock);
    kc_load();
    OSStatus st = CFDictionaryContainsKey(kc, k) ? errSecSuccess : errSecItemNotFound;
    CFDictionaryRemoveValue(kc, k);
    kc_save();
    pthread_mutex_unlock(&kc_lock);
    RET(st);
}

// SecKeychainItemModifyContent(item, attrList (ignored), length, data)
THUNK(SecKeychainItemModifyContent) {
    CFStringRef k = ARGSTR(0);
    if (!k || CFGetTypeID(k) != CFStringGetTypeID()) RET(errSecParam);
    if (ARG(1)) LOGD("security: ModifyContent attribute list ignored");
    if (!ARG(3)) RET(errSecSuccess);
    pthread_mutex_lock(&kc_lock);
    kc_load();
    CFDataRef d = CFDataCreate(NULL, ARGP(3), ARG(2));
    CFDictionarySetValue(kc, k, d);
    CFRelease(d);
    kc_save();
    pthread_mutex_unlock(&kc_lock);
    RET(errSecSuccess);
}

THUNK(SecRandomCopyBytes) {
    arc4random_buf(ARGP(2), ARG(1));
    RET(0);
}
