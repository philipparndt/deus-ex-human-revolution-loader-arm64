// IOKit registry/GPU queries, HID manager, RIPEMD-160, misc Security/CV/CM bits.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <Security/Security.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cf_rsa_data.h"

_Static_assert(sizeof(CVTimeStamp) == 80, "");
_Static_assert(offsetof(CVTimeStamp, smpteTime) == 40, "");
_Static_assert(sizeof(CMTime) == 24, "");

typedef struct { unsigned A, B, C, D, E, Nl, Nh, data[16], num; } RIPEMD160_CTX;
unsigned char *RIPEMD160(const unsigned char *d, unsigned long n, unsigned char *md);
int RIPEMD160_Init(RIPEMD160_CTX *c);
int RIPEMD160_Update(RIPEMD160_CTX *c, const void *data, unsigned long len);
int RIPEMD160_Final(unsigned char *md, RIPEMD160_CTX *c);
typedef struct { unsigned *d; int top, dmax, neg, flags; } BIGNUM;
typedef struct { int pad; long version; void *meth, *engine; BIGNUM *n, *e, *d, *p, *q, *dmp1, *dmq1, *iqmp; } RSA;
BIGNUM *BN_bin2bn(const unsigned char *s, int len, BIGNUM *ret);
RSA *RSA_new(void);
void RSA_free(RSA *r);
int RSA_verify(int type, const unsigned char *m, unsigned m_len, const unsigned char *sig, unsigned siglen, RSA *rsa);

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static void hex(const unsigned char *p, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]);
}

static int list_class(const char *cls, int show_vram) {
    io_iterator_t it = 0;
    CHECK(IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(cls), &it) == KERN_SUCCESS);
    int n = 0;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        io_name_t name;
        IORegistryEntryGetName(o, name);
        if (n < 3) {
            printf("  %s[%d] %s", cls, n, name);
            if (show_vram) {
                CFTypeRef v = IORegistryEntryCreateCFProperty(o, CFSTR("VRAM,totalMB"), kCFAllocatorDefault, 0);
                int mb = 0;
                if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &mb);
                printf(" VRAM,totalMB=%d", mb);
                if (n == 0) CHECK(mb > 0 && mb < 4096);
                if (v) CFRelease(v);
                CFTypeRef m = IORegistryEntryCreateCFProperty(o, CFSTR("model"), kCFAllocatorDefault, 0);
                if (m && CFGetTypeID(m) == CFDataGetTypeID()) printf(" model=%s", (const char *)CFDataGetBytePtr(m));
                if (m) CFRelease(m);
            }
            printf("\n");
        }
        IOObjectRelease(o);
        n++;
    }
    IOObjectRelease(it);
    printf("%s: %d entries\n", cls, n);
    return n;
}

int main(void) {
    unsigned char md[20];
    char h[41];
    RIPEMD160((const unsigned char *)"abc", 3, md);
    hex(md, 20, h);
    CHECK(strcmp(h, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc") == 0);
    RIPEMD160_CTX c;
    RIPEMD160_Init(&c);
    const char *msg = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    for (int i = 0; i < 8; i++) RIPEMD160_Update(&c, msg + 10 * i, 10);
    RIPEMD160_Final(md, &c);
    hex(md, 20, h);
    CHECK(strcmp(h, "9b752e45573d4b39f4dbd3323cab82bf63326bfb") == 0);
    printf("ripemd160 ok\n");
    RSA *rsa = RSA_new();
    static const unsigned char e65537[] = {1, 0, 1};
    rsa->n = BN_bin2bn(rsa_n, sizeof rsa_n, NULL);
    rsa->e = BN_bin2bn(e65537, 3, NULL);
    CHECK(RSA_verify(64, rsa_digest, 20, rsa_sig, sizeof rsa_sig, rsa) == 1);
    unsigned char bad[20];
    memcpy(bad, rsa_digest, 20);
    bad[0] ^= 1;
    CHECK(RSA_verify(64, bad, 20, rsa_sig, sizeof rsa_sig, rsa) == 0);
    RSA_free(rsa);

    CHECK(list_class("IOPCIDevice", 1) >= 1);
    list_class("IOAccelerator", 1);

    io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    CHECK(s != 0);
    CFTypeRef serial = IORegistryEntryCreateCFProperty(s, CFSTR("IOPlatformUUID"), NULL, 0);
    CHECK(serial && CFGetTypeID(serial) == CFStringGetTypeID());
    if (serial) CFRelease(serial);
    IOObjectRelease(s);

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CHECK(m != NULL && CFGetTypeID(m) == IOHIDManagerGetTypeID());
    IOHIDManagerSetDeviceMatching(m, NULL);
    IOReturn rc = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    CFSetRef devs = IOHIDManagerCopyDevices(m);
    printf("HID manager open -> %#x, %ld devices\n", rc, devs ? (long)CFSetGetCount(devs) : 0L);
    if (devs) CFRelease(devs);
    IOHIDManagerClose(m, 0);
    CFRelease(m);

    // CoreMedia time math.
    CMTime t = CMTimeMake(3, 2);
    CHECK(CMTimeGetSeconds(t) == 1.5);
    CHECK(CMTimeCompare(t, kCMTimeZero) == 1);
    CMTimeRange tr = CMTimeRangeMake(kCMTimeZero, t);
    CHECK(tr.duration.value == 3 && tr.duration.timescale == 2);

    // CVDisplayLink nominal refresh period.
    CVDisplayLinkRef dl = NULL;
    CHECK(CVDisplayLinkCreateWithActiveCGDisplays(&dl) == kCVReturnSuccess);
    if (dl) {
        CVTime p = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(dl);
        printf("display link refresh period %lld/%d\n", p.timeValue, p.timeScale);
        CVDisplayLinkRelease(dl);
    }

    // Keychain store (private file).
    const char *svc = "dxhost-test-service", *acct = "tester";
    SecKeychainItemRef item = NULL;
    UInt32 len = 0;
    void *data = NULL;
    if (SecKeychainFindGenericPassword(NULL, strlen(svc), svc, strlen(acct), acct, &len, &data, &item) == 0) {
        SecKeychainItemFreeContent(NULL, data);
        SecKeychainItemDelete(item);
        CFRelease(item);
    }
    CHECK(SecKeychainAddGenericPassword(NULL, strlen(svc), svc, strlen(acct), acct, 6, "secret", &item) == 0);
    CFRelease(item);
    CHECK(SecKeychainFindGenericPassword(NULL, strlen(svc), svc, strlen(acct), acct, &len, &data, &item) == 0);
    CHECK(len == 6 && memcmp(data, "secret", 6) == 0);
    SecKeychainItemFreeContent(NULL, data);
    CHECK(SecKeychainItemDelete(item) == 0);
    CFRelease(item);

    printf("cf_iokit: %s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
