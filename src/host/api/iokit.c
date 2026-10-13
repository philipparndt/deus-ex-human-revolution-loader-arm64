// IOKit: registry/services (io_object_t are mach port names, passed through),
// HID manager (CF objects + guest callbacks), notification ports.
//
// GPU detection: Apple Silicon has no PCI GPU and no "VRAM,*" properties. We
//  - insert a synthetic display-class IOPCIDevice at the front of every
//    IOPCIDevice match (fake object ids FAKE_BASE..),
//  - synthesise VRAM properties on any entry that lacks them when asked.
// Environment: DXH_VRAM_MB (default 2048), DXH_GPU_VENDOR / DXH_GPU_DEVICE (hex,
// default 0x106b / 0x0001), DXH_GPU_MODEL (default: host accelerator model).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include "ret_return.h"

// --- fake objects ------------------------------------------------------------------------

#define FAKE_BASE 0xFFFFF000u
#define FAKE_MAX 1024u
#define FAKE_GPU FAKE_BASE // the synthetic GPU entry (id never freed)

enum { FK_FREE, FK_GPU, FK_ITER };
static struct fake {
    int kind;
    uint32_t *items;
    uint32_t n, pos;
} fakes[FAKE_MAX] = {[0] = {FK_GPU, NULL, 0, 0}};
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

static struct fake *fake_get(uint32_t id) {
    if (id < FAKE_BASE || id - FAKE_BASE >= FAKE_MAX) return NULL;
    struct fake *f = &fakes[id - FAKE_BASE];
    return f->kind == FK_FREE ? NULL : f;
}

static uint32_t fake_iter_new(uint32_t *items, uint32_t n) {
    pthread_mutex_lock(&fake_lock);
    for (uint32_t i = 1; i < FAKE_MAX; i++)
        if (fakes[i].kind == FK_FREE) {
            fakes[i] = (struct fake){FK_ITER, items, n, 0};
            pthread_mutex_unlock(&fake_lock);
            return FAKE_BASE + i;
        }
    pthread_mutex_unlock(&fake_lock);
    fatal("iokit: too many synthetic iterators");
}

// --- GPU synthesis -----------------------------------------------------------------------------

static uint32_t env_u32(const char *name, uint32_t def, int base) {
    const char *v = getenv(name);
    return v && *v ? (uint32_t)strtoul(v, NULL, base) : def;
}
static uint32_t vram_mb(void) {
    uint32_t mb = env_u32("DXH_VRAM_MB", 2048, 10);
    return mb > 4095 ? 4095 : mb; // keep byte counts representable in 32 bits
}

static char *host_gpu_model(void) {
    static char model[128];
    if (model[0]) return model;
    const char *env = getenv("DXH_GPU_MODEL");
    if (env && *env) {
        strlcpy(model, env, sizeof model);
        return model;
    }
    strlcpy(model, "Apple GPU", sizeof model);
    io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    if (acc) {
        CFTypeRef m = IORegistryEntryCreateCFProperty(acc, CFSTR("model"), NULL, 0);
        if (m && CFGetTypeID(m) == CFStringGetTypeID()) CFStringGetCString(m, model, sizeof model, kCFStringEncodingUTF8);
        else if (m && CFGetTypeID(m) == CFDataGetTypeID())
            strlcpy(model, (const char *)CFDataGetBytePtr(m),
                    (size_t)CFDataGetLength(m) + 1 < sizeof model ? (size_t)CFDataGetLength(m) + 1 : sizeof model);
        if (m) CFRelease(m);
        IOObjectRelease(acc);
    }
    return model;
}

static CFDataRef data_u32(uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return CFDataCreate(NULL, b, 4);
}
static CFDataRef data_str(const char *s) { return CFDataCreate(NULL, (const UInt8 *)s, (CFIndex)strlen(s) + 1); }
static CFNumberRef num_i32(int32_t v) { return CFNumberCreate(NULL, kCFNumberSInt32Type, &v); }

// Synthetic VRAM-style property, or NULL if key is not one.
static CFTypeRef synth_vram(CFStringRef key) {
    uint32_t mb = vram_mb();
    if (CFEqual(key, CFSTR("VRAM,totalMB")) || CFEqual(key, CFSTR("VRAM,MB"))) return num_i32((int32_t)mb);
    if (CFEqual(key, CFSTR("VRAM,totalsize")) || CFEqual(key, CFSTR("ATY,memsize")) ||
        CFEqual(key, CFSTR("NVDA,memsize")))
        return data_u32(mb << 20);
    if (CFEqual(key, CFSTR("IOFBMemorySize")) || CFEqual(key, CFSTR("VRAM,totalBytes")))
        return num_i32((int32_t)(mb << 20));
    return NULL;
}

static CFMutableDictionaryRef gpu_props(void) {
    CFMutableDictionaryRef d =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
#define PUT(k, v)                                                                                  \
    do {                                                                                           \
        CFTypeRef v_ = (v);                                                                        \
        CFDictionarySetValue(d, CFSTR(k), v_);                                                     \
        CFRelease(v_);                                                                             \
    } while (0)
    PUT("IOName", CFStringCreateWithCString(NULL, "display", kCFStringEncodingUTF8));
    PUT("IOClass", CFStringCreateWithCString(NULL, "IOPCIDevice", kCFStringEncodingUTF8));
    PUT("name", data_str("display"));
    PUT("model", data_str(host_gpu_model()));
    PUT("vendor-id", data_u32(env_u32("DXH_GPU_VENDOR", 0x106b, 16)));
    PUT("device-id", data_u32(env_u32("DXH_GPU_DEVICE", 0x0001, 16)));
    PUT("class-code", data_u32(0x030000));
    PUT("VRAM,totalMB", synth_vram(CFSTR("VRAM,totalMB")));
    PUT("VRAM,totalsize", synth_vram(CFSTR("VRAM,totalsize")));
    PUT("IOFBMemorySize", synth_vram(CFSTR("IOFBMemorySize")));
#undef PUT
    return d;
}

// --- master port / matching ------------------------------------------------------------------------

static void init_master_port(gaddr_t a) { gwrite32(a, 0); }
GDATA(kIOMasterPortDefault, 4, init_master_port);
GDATA(kIOMainPortDefault, 4, init_master_port);

THUNK(IOMasterPort) {
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET(0);
}
THUNK(IOMainPort) {
    if (ARG(1)) gwrite32(ARG(1), 0);
    RET(0);
}

THUNK(IOServiceMatching) {
    LOGI("iokit: IOServiceMatching(\"%s\")", ARGS(0));
    RET_CF(IOServiceMatching(ARGS(0)));
}
THUNK(IOServiceNameMatching) {
    LOGI("iokit: IOServiceNameMatching(\"%s\")", ARGS(0));
    RET_CF(IOServiceNameMatching(ARGS(0)));
}
THUNK(IOBSDNameMatching) {
    LOGI("iokit: IOBSDNameMatching(\"%s\")", ARGS(2));
    RET_CF(IOBSDNameMatching(ARG(0), ARG(1), ARGS(2)));
}

static bool matches_pci(CFDictionaryRef m) {
    if (!m) return false;
    CFTypeRef c = CFDictionaryGetValue(m, CFSTR(kIOProviderClassKey));
    return c && CFGetTypeID(c) == CFStringGetTypeID() && CFEqual(c, CFSTR("IOPCIDevice"));
}

static void log_matching(const char *fn, CFDictionaryRef m) {
    if (log_level < 1 || !m) return;
    CFStringRef d = CFCopyDescription(m);
    char *s = cf_utf8(d);
    LOGI("iokit: %s %s", fn, s);
    free(s);
    CFRelease(d);
}

// IOServiceGetMatchingServices(mach_port_t master, CFDictionaryRef matching (consumed), io_iterator_t *it)
THUNK(IOServiceGetMatchingServices) {
    CFDictionaryRef m = (CFDictionaryRef)ARGCF(1);
    log_matching("IOServiceGetMatchingServices", m);
    bool pci = matches_pci(m);
    io_iterator_t it = 0;
    kern_return_t kr = IOServiceGetMatchingServices(ARG(0), m, &it);
    if (kr != KERN_SUCCESS) {
        if (ARG(2)) gwrite32(ARG(2), 0);
        RET(kr);
    }
    if (!pci) {
        gwrite32(ARG(2), it);
        RET(kr);
    }
    // Put the synthetic GPU first, then the real devices.
    uint32_t cap = 16, n = 0, *items = malloc(cap * 4);
    items[n++] = FAKE_GPU;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        if (n == cap) items = realloc(items, (cap *= 2) * 4);
        items[n++] = o;
    }
    IOObjectRelease(it);
    gwrite32(ARG(2), fake_iter_new(items, n));
    LOGI("iokit: IOPCIDevice match -> synthetic GPU + %u host devices", n - 1);
    RET(KERN_SUCCESS);
}

THUNK(IOServiceGetMatchingService) {
    CFDictionaryRef m = (CFDictionaryRef)ARGCF(1);
    log_matching("IOServiceGetMatchingService", m);
    if (matches_pci(m)) {
        if (m) CFRelease(m);
        RET(FAKE_GPU);
    }
    RET(IOServiceGetMatchingService(ARG(0), m));
}

THUNK(IOIteratorNext) {
    struct fake *f = fake_get(ARG(0));
    if (f && f->kind == FK_ITER) RET(f->pos < f->n ? f->items[f->pos++] : 0);
    if (f) RET(0);
    RET(IOIteratorNext(ARG(0)));
}
THUNK(IOIteratorReset) {
    struct fake *f = fake_get(ARG(0));
    if (f) {
        f->pos = 0;
        return;
    }
    IOIteratorReset(ARG(0));
}
THUNK(IOIteratorIsValid) {
    if (fake_get(ARG(0))) RET(1);
    RET(IOIteratorIsValid(ARG(0)));
}

THUNK(IOObjectRelease) {
    uint32_t id = ARG(0);
    if (id >= FAKE_BASE && id - FAKE_BASE < FAKE_MAX) {
        struct fake *f = fake_get(id);
        if (f && f->kind == FK_ITER) {
            pthread_mutex_lock(&fake_lock);
            for (uint32_t i = f->pos; i < f->n; i++)
                if (f->items[i] < FAKE_BASE) IOObjectRelease(f->items[i]);
            free(f->items);
            f->kind = FK_FREE;
            pthread_mutex_unlock(&fake_lock);
        }
        RET(0);
    }
    RET(IOObjectRelease(id));
}
THUNK(IOObjectRetain) {
    if (ARG(0) >= FAKE_BASE) RET(0);
    RET(IOObjectRetain(ARG(0)));
}

static const char *fake_class(uint32_t id) { return fake_get(id) && fake_get(id)->kind == FK_GPU ? "IOPCIDevice" : "IOIterator"; }

THUNK(IOObjectConformsTo) {
    if (fake_get(ARG(0))) {
        const char *c = ARGS(1);
        bool yes = fake_get(ARG(0))->kind == FK_GPU
                       ? (!strcmp(c, "IOPCIDevice") || !strcmp(c, "IOService") || !strcmp(c, "IORegistryEntry"))
                       : !strcmp(c, "IOIterator");
        RET(yes);
    }
    RET(IOObjectConformsTo(ARG(0), ARGS(1)));
}
THUNK(IOObjectGetClass) {
    if (fake_get(ARG(0))) {
        gstrlcpy(ARG(1), fake_class(ARG(0)), 128);
        RET(0);
    }
    io_name_t n = "";
    kern_return_t kr = IOObjectGetClass(ARG(0), n);
    gstrlcpy(ARG(1), n, 128);
    RET(kr);
}
THUNK(IOObjectIsEqualTo) { RET(ARG(0) == ARG(1) || IOObjectIsEqualTo(ARG(0), ARG(1))); }

// --- registry ------------------------------------------------------------------------------------

THUNK(IORegistryEntryGetName) {
    if (fake_get(ARG(0))) {
        gstrlcpy(ARG(1), "display", 128);
        RET(0);
    }
    io_name_t n = "";
    kern_return_t kr = IORegistryEntryGetName(ARG(0), n);
    gstrlcpy(ARG(1), n, 128);
    RET(kr);
}
THUNK(IORegistryEntryGetNameInPlane) {
    if (fake_get(ARG(0))) {
        gstrlcpy(ARG(2), "display", 128);
        RET(0);
    }
    io_name_t n = "";
    kern_return_t kr = IORegistryEntryGetNameInPlane(ARG(0), ARGS(1), n);
    gstrlcpy(ARG(2), n, 128);
    RET(kr);
}

THUNK(IORegistryEntryFromPath) {
    LOGI("iokit: IORegistryEntryFromPath(\"%s\")", ARGS(1));
    RET(IORegistryEntryFromPath(ARG(0), ARGS(1)));
}

THUNK(IORegistryEntryGetParentEntry) {
    if (fake_get(ARG(0))) {
        // Parent of the synthetic GPU: the registry root.
        gwrite32(ARG(2), IORegistryGetRootEntry(kIOMainPortDefault));
        RET(0);
    }
    io_registry_entry_t p = 0;
    kern_return_t kr = IORegistryEntryGetParentEntry(ARG(0), ARGS(1), &p);
    gwrite32(ARG(2), p);
    RET(kr);
}
THUNK(IORegistryGetRootEntry) { RET(IORegistryGetRootEntry(ARG(0))); }

// IORegistryEntryCreateIterator(entry, plane, options, io_iterator_t *it)
THUNK(IORegistryEntryCreateIterator) {
    if (fake_get(ARG(0))) {
        gwrite32(ARG(3), fake_iter_new(NULL, 0));
        RET(0);
    }
    io_iterator_t it = 0;
    kern_return_t kr = IORegistryEntryCreateIterator(ARG(0), ARGS(1), ARG(2), &it);
    gwrite32(ARG(3), it);
    RET(kr);
}
THUNK(IORegistryEntryGetChildIterator) {
    if (fake_get(ARG(0))) {
        gwrite32(ARG(2), fake_iter_new(NULL, 0));
        RET(0);
    }
    io_iterator_t it = 0;
    kern_return_t kr = IORegistryEntryGetChildIterator(ARG(0), ARGS(1), &it);
    gwrite32(ARG(2), it);
    RET(kr);
}

static void entry_class(uint32_t e, char *buf) {
    if (fake_get(e)) strcpy(buf, fake_class(e));
    else if (IOObjectGetClass(e, buf) != KERN_SUCCESS) strcpy(buf, "?");
}

static bool gpu_like(uint32_t e) {
    static const char *const cls[] = {"IOAccelerator", "IOFramebuffer", "IOMobileFramebuffer", "IODisplay",
                                      "IONDRVFramebuffer"};
    for (size_t i = 0; i < sizeof cls / sizeof *cls; i++)
        if (IOObjectConformsTo(e, cls[i])) return true;
    return false;
}

static CFTypeRef entry_property(uint32_t e, CFStringRef key) {
    if (!key) return NULL;
    char cls[128], *k = cf_utf8(key);
    entry_class(e, cls);
    CFTypeRef v;
    bool synth = false;
    if (fake_get(e)) {
        CFDictionaryRef d = gpu_props();
        v = CFDictionaryGetValue(d, key);
        if (v) CFRetain(v);
        CFRelease(d);
        synth = true;
    } else {
        v = IORegistryEntryCreateCFProperty(e, key, NULL, 0);
        // VRAM keys: on GPU-like entries (or wherever the host has a value, which
        // may exceed what a 32-bit game can represent) answer the synthetic size.
        CFTypeRef sv = synth_vram(key);
        if (sv && (v || gpu_like(e))) {
            if (v) CFRelease(v);
            v = sv;
            synth = true;
        } else if (sv)
            CFRelease(sv);
    }
    LOGI("iokit: property \"%s\" of %s entry %#x -> %s%s", k, cls, e, v ? "found" : "none",
         synth ? " (synthesised)" : "");
    free(k);
    return v;
}

// IORegistryEntryCreateCFProperty(entry, CFStringRef key, CFAllocatorRef, IOOptionBits)
THUNK(IORegistryEntryCreateCFProperty) { RET_CF(entry_property(ARG(0), ARGSTR(1))); }

// IORegistryEntrySearchCFProperty(entry, plane, key, allocator, options)
THUNK(IORegistryEntrySearchCFProperty) {
    CFStringRef key = ARGSTR(2);
    if (fake_get(ARG(0))) RET_CF(entry_property(ARG(0), key));
    CFTypeRef v = IORegistryEntrySearchCFProperty(ARG(0), ARGS(1), key, NULL, ARG(4));
    CFTypeRef sv = synth_vram(key);
    if (sv && (v || gpu_like(ARG(0)))) {
        if (v) CFRelease(v);
        v = sv;
    } else if (sv)
        CFRelease(sv);
    char *k = cf_utf8(key);
    LOGI("iokit: search property \"%s\" from %#x -> %s", k, ARG(0), v ? "found" : "none");
    free(k);
    RET_CF(v);
}

// IORegistryEntryCreateCFProperties(entry, CFMutableDictionaryRef *props, allocator, options)
THUNK(IORegistryEntryCreateCFProperties) {
    CFMutableDictionaryRef d = NULL;
    kern_return_t kr = 0;
    if (fake_get(ARG(0))) d = gpu_props();
    else kr = IORegistryEntryCreateCFProperties(ARG(0), &d, NULL, ARG(3));
    LOGI("iokit: all properties of %#x", ARG(0));
    gwrite32(ARG(1), cfg(d));
    RET(kr);
}

THUNK(IOCreatePlugInInterfaceForService) {
    // Returns a host COM vtable the guest cannot call; unsupported.
    LOGI("iokit: IOCreatePlugInInterfaceForService(%#x) -> unsupported", ARG(0));
    if (ARG(3)) gwrite32(ARG(3), 0);
    if (ARG(4)) gwrite32(ARG(4), 0);
    RET(kIOReturnUnsupported);
}

// --- notifications -----------------------------------------------------------------------------------

THUNK(IONotificationPortCreate) { RET(hobj_wrap(IONotificationPortCreate(ARG(0)))); }
THUNK(IONotificationPortDestroy) {
    IONotificationPortRef p = hobj_unwrap(ARG(0));
    if (p) IONotificationPortDestroy(p);
    hobj_forget(ARG(0));
}
THUNK(IONotificationPortGetRunLoopSource) {
    IONotificationPortRef p = hobj_unwrap(ARG(0));
    RET_CF(p ? IONotificationPortGetRunLoopSource(p) : NULL);
}

struct gnotify {
    gaddr_t fn, refcon;
};
static void notify_cb(void *refcon, io_iterator_t it) {
    struct gnotify *n = refcon;
    gcall2(n->fn, n->refcon, it);
}

// IOServiceAddMatchingNotification(port, type, matching (consumed), callback, refCon, io_iterator_t *it)
THUNK(IOServiceAddMatchingNotification) {
    IONotificationPortRef p = hobj_unwrap(ARG(0));
    LOGI("iokit: IOServiceAddMatchingNotification(%s)", ARGS(1));
    log_matching("  matching", (CFDictionaryRef)ARGCF(2));
    struct gnotify *n = malloc(sizeof *n); // lives as long as the notification (leaked)
    n->fn = ARG(3);
    n->refcon = ARG(4);
    io_iterator_t it = 0;
    kern_return_t kr = IOServiceAddMatchingNotification(p, ARGS(1), (CFDictionaryRef)ARGCF(2), notify_cb, n, &it);
    gwrite32(ARG(5), it);
    RET(kr);
}

// --- power management ----------------------------------------------------------------------------------
#include <IOKit/pwr_mgt/IOPMLib.h>

THUNK(IOPMAssertionCreateWithName) {
    IOPMAssertionID id = 0;
    IOReturn rc = IOPMAssertionCreateWithName(ARGSTR(0), ARG(1), ARGSTR(2), &id);
    gwrite32(ARG(3), id);
    RET(rc);
}
THUNK(IOPMAssertionCreate) {
    IOPMAssertionID id = 0;
    IOReturn rc = IOPMAssertionCreateWithName(ARGSTR(0), ARG(1), CFSTR("dxhost game"), &id);
    gwrite32(ARG(2), id);
    RET(rc);
}
THUNK(IOPMAssertionRelease) { RET(IOPMAssertionRelease(ARG(0))); }

// --- HID manager -------------------------------------------------------------------------------------------

struct ghid_cb {
    gaddr_t fn, ctx;
};
static char hid_match_key, hid_remove_key, hid_value_key;

static void hid_device_cb(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    struct ghid_cb *c = context;
    uint32_t a[4] = {c->ctx, (uint32_t)result, cfg(sender), cfg(device)};
    cpu_call(c->fn, 4, a);
}
static bool bot_delivering; // autoplay value being delivered (game thread)
// autoplay: the HID manager with the game's value callback, where it is scheduled
static IOHIDManagerRef bot_mgr;
static struct ghid_cb *bot_cb;
static CFRunLoopRef bot_rl;
static pthread_mutex_t bot_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    IOHIDManagerRef m;
    CFRunLoopRef rl;
} bot_sched[8]; // retained run loops of scheduled managers

static IOHIDManagerRef bot_km; // the manager matching keyboards / mice (not retained: identity only)

// Does a device matching dictionary ask for a keyboard or mouse?
static bool matches_keyboard_mouse(CFDictionaryRef d) {
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return false;
    int32_t pg = 0, us = 0;
    CFNumberRef n1 = CFDictionaryGetValue(d, CFSTR(kIOHIDDeviceUsagePageKey));
    CFNumberRef n2 = CFDictionaryGetValue(d, CFSTR(kIOHIDDeviceUsageKey));
    if (n1 && CFGetTypeID(n1) == CFNumberGetTypeID()) CFNumberGetValue(n1, kCFNumberSInt32Type, &pg);
    if (n2 && CFGetTypeID(n2) == CFNumberGetTypeID()) CFNumberGetValue(n2, kCFNumberSInt32Type, &us);
    return pg == kHIDPage_GenericDesktop && (us == kHIDUsage_GD_Keyboard || us == kHIDUsage_GD_Mouse);
}

static void bot_update(void) { // bot_lock held
    CFRunLoopRef rl = NULL;
    for (int i = 0; i < 8; i++)
        if (bot_sched[i].m && bot_sched[i].m == bot_mgr) rl = bot_sched[i].rl;
    if (rl) CFRetain(rl);
    if (bot_rl) CFRelease(bot_rl);
    bot_rl = rl;
}

static void hid_value_cb(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    extern bool cb_bot; // carbon_event.m: autoplay has the controls
    if (cb_bot && !bot_delivering) return;
    struct ghid_cb *c = context;
    uint32_t a[4] = {c->ctx, (uint32_t)result, cfg(sender), cfg(value)};
    cpu_call(c->fn, 4, a);
}

static struct ghid_cb *hid_cb(CFTypeRef owner, const void *key, gaddr_t fn, gaddr_t ctx) {
    if (!fn) {
        cf_attach(owner, key, NULL, NULL);
        return NULL;
    }
    struct ghid_cb *c = malloc(sizeof *c);
    c->fn = fn;
    c->ctx = ctx;
    cf_attach(owner, key, c, free);
    return c;
}

THUNK(IOHIDManagerCreate) { RET_CF(IOHIDManagerCreate(gcf_allocator(ARG(0)), ARG(1))); }
THUNK(IOHIDManagerGetTypeID) { RET(IOHIDManagerGetTypeID()); }
THUNK(IOHIDManagerOpen) {
    IOReturn rc = IOHIDManagerOpen((IOHIDManagerRef)ARGCF(0), ARG(1));
    LOGI("iokit: IOHIDManagerOpen -> %#x", rc);
    extern bool cb_bot; // carbon_event.m
    if (rc && cb_bot && ARGCF(0) == bot_km) {
        // No Input Monitoring permission (started in the background): the game
        // would give up on keyboard/mouse; autoplay feeds this manager instead.
        LOGI("iokit: autoplay: keyboard/mouse manager open reported as successful");
        rc = kIOReturnSuccess;
    }
    RET(rc);
}
THUNK(IOHIDManagerClose) { RET(IOHIDManagerClose((IOHIDManagerRef)ARGCF(0), ARG(1))); }
THUNK(IOHIDManagerSetDeviceMatching) {
    IOHIDManagerSetDeviceMatching((IOHIDManagerRef)ARGCF(0), (CFDictionaryRef)ARGCF(1));
    if (matches_keyboard_mouse((CFDictionaryRef)ARGCF(1))) bot_km = (IOHIDManagerRef)ARGCF(0);
}
THUNK(IOHIDManagerSetDeviceMatchingMultiple) {
    CFArrayRef ms = (CFArrayRef)ARGCF(1);
    for (CFIndex i = 0; ms && i < CFArrayGetCount(ms); i++) {
        log_matching("IOHIDManagerSetDeviceMatchingMultiple", CFArrayGetValueAtIndex(ms, i));
        if (matches_keyboard_mouse(CFArrayGetValueAtIndex(ms, i))) bot_km = (IOHIDManagerRef)ARGCF(0);
    }
    IOHIDManagerSetDeviceMatchingMultiple((IOHIDManagerRef)ARGCF(0), (CFArrayRef)ARGCF(1));
}
THUNK(IOHIDManagerCopyDevices) { RET_CF(IOHIDManagerCopyDevices((IOHIDManagerRef)ARGCF(0))); }
THUNK(IOHIDManagerScheduleWithRunLoop) {
    IOHIDManagerScheduleWithRunLoop((IOHIDManagerRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2));
    pthread_mutex_lock(&bot_lock); // autoplay: where its HID values are delivered
    for (int i = 0; i < 8; i++)
        if (!bot_sched[i].m) {
            bot_sched[i].m = (IOHIDManagerRef)CFRetain(ARGCF(0));
            bot_sched[i].rl = (CFRunLoopRef)CFRetain(ARGCF(1));
            break;
        }
    bot_update();
    pthread_mutex_unlock(&bot_lock);
}
THUNK(IOHIDManagerUnscheduleFromRunLoop) {
    IOHIDManagerUnscheduleFromRunLoop((IOHIDManagerRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2));
    pthread_mutex_lock(&bot_lock);
    for (int i = 0; i < 8; i++)
        if (bot_sched[i].m == ARGCF(0) && bot_sched[i].rl == ARGCF(1)) {
            CFRelease(bot_sched[i].m), CFRelease(bot_sched[i].rl);
            bot_sched[i].m = NULL, bot_sched[i].rl = NULL;
        }
    bot_update();
    pthread_mutex_unlock(&bot_lock);
}
THUNK(IOHIDManagerRegisterDeviceMatchingCallback) {
    IOHIDManagerRef m = (IOHIDManagerRef)ARGCF(0);
    struct ghid_cb *c = hid_cb(m, &hid_match_key, ARG(1), ARG(2));
    IOHIDManagerRegisterDeviceMatchingCallback(m, c ? hid_device_cb : NULL, c);
}
THUNK(IOHIDManagerRegisterDeviceRemovalCallback) {
    IOHIDManagerRef m = (IOHIDManagerRef)ARGCF(0);
    struct ghid_cb *c = hid_cb(m, &hid_remove_key, ARG(1), ARG(2));
    IOHIDManagerRegisterDeviceRemovalCallback(m, c ? hid_device_cb : NULL, c);
}
// The game reads in-game keyboard/mouse through this callback; autoplay
// (autoplay.m) feeds it values too.

THUNK(IOHIDManagerRegisterInputValueCallback) {
    IOHIDManagerRef m = (IOHIDManagerRef)ARGCF(0);
    struct ghid_cb *c = hid_cb(m, &hid_value_key, ARG(1), ARG(2));
    IOHIDManagerRegisterInputValueCallback(m, c ? hid_value_cb : NULL, c);
    pthread_mutex_lock(&bot_lock);
    if (c && bot_km && m != bot_km) {
        // a gamepad manager: not where keyboard/mouse values belong
    } else if (c) {
        CFRetain(m);
        if (bot_mgr) CFRelease(bot_mgr);
        bot_mgr = m, bot_cb = c;
    } else if (bot_mgr == m) {
        CFRelease(bot_mgr);
        bot_mgr = NULL, bot_cb = NULL;
    }
    bot_update();
    pthread_mutex_unlock(&bot_lock);
}

// Without Input Monitoring permission (e.g. when started from a background
// process) IOKit gives the devices no elements and IOHIDElementCreateWithDictionary
// makes empty ones, so autoplay values and their elements are marked CF
// dictionaries instead; the IOHIDValue / IOHIDElement thunks below answer for
// them. The device is the real keyboard or mouse the game was told about.
//   element: {dxbot, page, usage, cookie, device}   value: {dxbot, element, value, time}
static bool bot_obj(CFTypeRef o) {
    return o && CFGetTypeID(o) == CFDictionaryGetTypeID() && CFDictionaryContainsKey(o, CFSTR("dxbot"));
}
static int64_t bot_num(CFTypeRef o, CFStringRef key) {
    CFNumberRef n = CFDictionaryGetValue(o, key);
    int64_t v = 0;
    if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

// The game's keyboard (generic desktop usage 6) or mouse (2), by primary usage:
// IOHIDDeviceConformsTo needs the permission too.
static IOHIDDeviceRef bot_device(IOHIDManagerRef m, uint32_t gd_usage) {
    IOHIDDeviceRef dev = NULL;
    CFSetRef devs = IOHIDManagerCopyDevices(m);
    CFIndex count = devs ? CFSetGetCount(devs) : 0;
    const void **d = malloc(sizeof *d * (count ? count : 1));
    if (devs) CFSetGetValues(devs, d);
    for (CFIndex i = 0; i < count && !dev; i++) {
        int32_t pg = 0, us = 0;
        CFNumberRef n1 = IOHIDDeviceGetProperty((IOHIDDeviceRef)d[i], CFSTR(kIOHIDPrimaryUsagePageKey));
        CFNumberRef n2 = IOHIDDeviceGetProperty((IOHIDDeviceRef)d[i], CFSTR(kIOHIDPrimaryUsageKey));
        if (n1) CFNumberGetValue(n1, kCFNumberSInt32Type, &pg);
        if (n2) CFNumberGetValue(n2, kCFNumberSInt32Type, &us);
        if (pg == kHIDPage_GenericDesktop && (uint32_t)us == gd_usage) dev = (IOHIDDeviceRef)d[i];
    }
    if (dev) CFRetain(dev);
    free(d);
    if (devs) CFRelease(devs);
    return dev;
}

static CFDictionaryRef bot_element(IOHIDManagerRef m, uint32_t page, uint32_t usage) {
    static struct {
        uint32_t page, usage;
        CFDictionaryRef el;
    } cache[128];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].page == page && cache[i].usage == usage) return cache[i].el;
    IOHIDDeviceRef dev = bot_device(m, page == kHIDPage_KeyboardOrKeypad ? kHIDUsage_GD_Keyboard : kHIDUsage_GD_Mouse);
    if (!dev || n == 128) {
        LOGE("iokit: autoplay: no %s device for usage page %#x usage %#x",
             page == kHIDPage_KeyboardOrKeypad ? "keyboard" : "mouse", page, usage);
        return NULL; // not cached: devices may still appear
    }
    int64_t pg = page, us = usage, cookie = 0x7000 + n;
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt64Type, &pg), nu = CFNumberCreate(NULL, kCFNumberSInt64Type, &us),
                nc = CFNumberCreate(NULL, kCFNumberSInt64Type, &cookie);
    const void *keys[] = {CFSTR("dxbot"), CFSTR("page"), CFSTR("usage"), CFSTR("cookie"), CFSTR("device")};
    const void *vals[] = {kCFBooleanTrue, np, nu, nc, dev};
    CFDictionaryRef el =
        CFDictionaryCreate(NULL, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(np), CFRelease(nu), CFRelease(nc), CFRelease(dev);
    cache[n].page = page, cache[n].usage = usage, cache[n].el = el, n++;
    return el;
}

// Autoplay: deliver a value for (usage page, usage) to the game's HID callback
// on the run loop the manager is scheduled on. False if there is no callback yet.
bool hid_bot_value(uint32_t page, uint32_t usage, int32_t value) {
    pthread_mutex_lock(&bot_lock);
    IOHIDManagerRef m = bot_mgr ? (IOHIDManagerRef)CFRetain(bot_mgr) : NULL;
    CFRunLoopRef rl = bot_rl ? (CFRunLoopRef)CFRetain(bot_rl) : NULL;
    pthread_mutex_unlock(&bot_lock);
    CFDictionaryRef el = m && rl ? bot_element(m, page, usage) : NULL;
    static int traced;
    if (getenv("DXH_BOT_TRACE") && traced < 40) {
        traced++;
        LOGI("bot-trace: hid value page %#x usage %#x = %d: manager %p run loop %p element %p", page, usage, value, m,
             rl, el);
    }
    if (!el) {
        if (m) CFRelease(m);
        if (rl) CFRelease(rl);
        return false;
    }
    int64_t v64 = value, t64 = (int64_t)mach_absolute_time();
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt64Type, &v64), nt = CFNumberCreate(NULL, kCFNumberSInt64Type, &t64);
    const void *keys[] = {CFSTR("dxbot"), CFSTR("element"), CFSTR("value"), CFSTR("time")};
    const void *vals[] = {kCFBooleanTrue, el, nv, nt};
    CFDictionaryRef v =
        CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(nv), CFRelease(nt);
    CFRunLoopPerformBlock(rl, kCFRunLoopCommonModes, ^{
      // on the game's thread: the callback is only replaced or removed here too
      static int traced_cb;
      if (getenv("DXH_BOT_TRACE") && traced_cb < 40) {
          traced_cb++;
          LOGI("bot-trace: hid callback on %p: manager %s, callback %p", CFRunLoopGetCurrent(),
               bot_mgr == m ? "current" : "replaced", bot_cb);
      }
      if (bot_mgr == m && bot_cb) {
          bot_delivering = true;
          hid_value_cb(bot_cb, kIOReturnSuccess, m, (IOHIDValueRef)v);
          bot_delivering = false;
      }
      CFRelease(v);
      CFRelease(m);
    });
    CFRunLoopWakeUp(rl);
    CFRelease(rl);
    extern void cb_wake_main(void); // carbon_event.m: the game may be waiting for AppKit events
    cb_wake_main();
    return true;
}

THUNK(IOHIDDeviceGetTypeID) { RET(IOHIDDeviceGetTypeID()); }
THUNK(IOHIDDeviceGetProperty) { RET_CF(IOHIDDeviceGetProperty((IOHIDDeviceRef)ARGCF(0), ARGSTR(1))); }
THUNK(IOHIDDeviceOpen) { RET(IOHIDDeviceOpen((IOHIDDeviceRef)ARGCF(0), ARG(1))); }
THUNK(IOHIDDeviceClose) { RET(IOHIDDeviceClose((IOHIDDeviceRef)ARGCF(0), ARG(1))); }
THUNK(IOHIDDeviceScheduleWithRunLoop) {
    IOHIDDeviceScheduleWithRunLoop((IOHIDDeviceRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2));
}
THUNK(IOHIDDeviceUnscheduleFromRunLoop) {
    IOHIDDeviceUnscheduleFromRunLoop((IOHIDDeviceRef)ARGCF(0), (CFRunLoopRef)ARGCF(1), ARGSTR(2));
}
THUNK(IOHIDDeviceCopyMatchingElements) {
    CFArrayRef els = IOHIDDeviceCopyMatchingElements((IOHIDDeviceRef)ARGCF(0), (CFDictionaryRef)ARGCF(1), ARG(2));
    LOGI("iokit: IOHIDDeviceCopyMatchingElements(%p) -> %ld elements", ARGCF(0), els ? (long)CFArrayGetCount(els) : -1L);
    RET_CF(els);
}
THUNK(IOHIDDeviceConformsTo) { RET_BOOL(IOHIDDeviceConformsTo((IOHIDDeviceRef)ARGCF(0), ARG(1), ARG(2))); }
THUNK(IOHIDDeviceGetService) { RET(IOHIDDeviceGetService((IOHIDDeviceRef)ARGCF(0))); }
// IOHIDDeviceGetValue(device, element, IOHIDValueRef *value)
THUNK(IOHIDDeviceGetValue) {
    IOHIDValueRef v = NULL;
    IOReturn rc = IOHIDDeviceGetValue((IOHIDDeviceRef)ARGCF(0), (IOHIDElementRef)ARGCF(1), &v);
    gwrite32(ARG(2), cfg(v));
    RET(rc);
}

#define BOT(i) bot_obj(ARGCF(i)) // autoplay element / value (see bot_element)
THUNK(IOHIDElementGetTypeID) { RET(IOHIDElementGetTypeID()); }
THUNK(IOHIDElementGetCookie) {
    if (BOT(0)) RET((uint32_t)bot_num(ARGCF(0), CFSTR("cookie")));
    else RET(IOHIDElementGetCookie((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetDevice) {
    if (BOT(0)) RET_CF(CFDictionaryGetValue(ARGCF(0), CFSTR("device")));
    else RET_CF(IOHIDElementGetDevice((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetUsage) {
    if (BOT(0)) RET((uint32_t)bot_num(ARGCF(0), CFSTR("usage")));
    else RET(IOHIDElementGetUsage((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetUsagePage) {
    if (BOT(0)) RET((uint32_t)bot_num(ARGCF(0), CFSTR("page")));
    else RET(IOHIDElementGetUsagePage((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetType) {
    if (BOT(0))
        RET(bot_num(ARGCF(0), CFSTR("page")) == kHIDPage_GenericDesktop ? kIOHIDElementTypeInput_Misc
                                                                        : kIOHIDElementTypeInput_Button);
    else RET(IOHIDElementGetType((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetLogicalMin) {
    if (BOT(0)) RET(bot_num(ARGCF(0), CFSTR("page")) == kHIDPage_GenericDesktop ? (uint32_t)-127 : 0);
    else RET((uint32_t)IOHIDElementGetLogicalMin((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetLogicalMax) {
    if (BOT(0)) RET(bot_num(ARGCF(0), CFSTR("page")) == kHIDPage_GenericDesktop ? 127 : 1);
    else RET((uint32_t)IOHIDElementGetLogicalMax((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetParent) {
    if (BOT(0)) RET_CF(NULL);
    else RET_CF(IOHIDElementGetParent((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetChildren) {
    if (BOT(0)) RET_CF(NULL);
    else RET_CF(IOHIDElementGetChildren((IOHIDElementRef)ARGCF(0)));
}
THUNK(IOHIDElementGetProperty) {
    if (BOT(0)) RET_CF(NULL);
    else RET_CF(IOHIDElementGetProperty((IOHIDElementRef)ARGCF(0), ARGSTR(1)));
}

THUNK(IOHIDValueGetTypeID) { RET(IOHIDValueGetTypeID()); }
THUNK(IOHIDValueGetElement) {
    if (BOT(0)) RET_CF(CFDictionaryGetValue(ARGCF(0), CFSTR("element")));
    else RET_CF(IOHIDValueGetElement((IOHIDValueRef)ARGCF(0)));
}
THUNK(IOHIDValueGetIntegerValue) {
    if (BOT(0)) RET((uint32_t)bot_num(ARGCF(0), CFSTR("value")));
    else RET((uint32_t)IOHIDValueGetIntegerValue((IOHIDValueRef)ARGCF(0)));
}
THUNK(IOHIDValueGetTimeStamp) {
    if (BOT(0)) RET64((uint64_t)bot_num(ARGCF(0), CFSTR("time")));
    else RET64(IOHIDValueGetTimeStamp((IOHIDValueRef)ARGCF(0)));
}
THUNK(IOHIDValueGetLength) {
    if (BOT(0)) RET(4);
    else RET(IOHIDValueGetLength((IOHIDValueRef)ARGCF(0)));
}
// double IOHIDValueGetScaledValue(value, IOHIDValueScaleType)
THUNK(IOHIDValueGetScaledValue) {
    if (BOT(0)) RETD((double)bot_num(ARGCF(0), CFSTR("value")));
    else RETD(IOHIDValueGetScaledValue((IOHIDValueRef)ARGCF(0), ARG(1)));
}
