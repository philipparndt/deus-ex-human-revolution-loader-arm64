// CoreFoundation: CFArray, CFDictionary, CFSet, CFData, CFTree.
//
// Element representation: with kCFType* callbacks elements are CF objects
// (converted guest<->host per element). With NULL callbacks they are raw 32-bit
// guest words stored as (void *)(uintptr_t)word; such containers carry raw
// flags (cf_set_raw_flags). Custom guest callbacks are supported through a
// small pool of host trampolines that call into the guest; elements are raw.
#include "cf_bridge.h"

#include <pthread.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// --- guest callback structs ------------------------------------------------------------
// i386: CFArrayCallBacks / CFDictionaryValueCallBacks = {version, retain, release,
// copyDescription, equal} (20 bytes); CFDictionaryKeyCallBacks / CFSetCallBacks
// add hash (24 bytes).

static void cb_retain(struct guest_regs *r) { // const void *(*)(CFAllocatorRef, const void *)
    CFTypeRef h = gcf(ARG(1));
    if (h && !cf_is_guest_constant(ARG(1))) CFRetain(h);
    RET(ARG(1));
}
static void cb_release(struct guest_regs *r) {
    CFTypeRef h = gcf(ARG(1));
    if (h && !cf_is_guest_constant(ARG(1))) CFRelease(h);
}
static void cb_copystr(struct guest_regs *r) {
    CFTypeRef h = gcf(ARG(1));
    RET_CF(h ? CFStringCreateCopy(NULL, h) : NULL);
}

static gaddr_t stub_retain, stub_release, stub_copystr;
static pthread_once_t stubs_once = PTHREAD_ONCE_INIT;
static void make_stubs(void) {
    stub_retain = thunk_register_dynamic("__dxh_cf_cb_retain", cb_retain);
    stub_release = thunk_register_dynamic("__dxh_cf_cb_release", cb_release);
    stub_copystr = thunk_register_dynamic("__dxh_cf_cb_copystring", cb_copystr);
}

static void write_cb(gaddr_t a, bool copystr, bool hash) {
    pthread_once(&stubs_once, make_stubs);
    gwrite32(a, 0);
    gwrite32(a + 4, copystr ? stub_copystr : stub_retain);
    gwrite32(a + 8, stub_release);
    gwrite32(a + 12, hostsym_lookup("_CFCopyDescription"));
    gwrite32(a + 16, hostsym_lookup("_CFEqual"));
    if (hash) gwrite32(a + 20, hostsym_lookup("_CFHash"));
}
static void init_type_cb20(gaddr_t a) { write_cb(a, false, false); }
static void init_type_cb24(gaddr_t a) { write_cb(a, false, true); }
static void init_copystr_cb24(gaddr_t a) { write_cb(a, true, true); }
GDATA(kCFTypeArrayCallBacks, 20, init_type_cb20);
GDATA(kCFTypeDictionaryValueCallBacks, 20, init_type_cb20);
GDATA(kCFTypeDictionaryKeyCallBacks, 24, init_type_cb24);
GDATA(kCFCopyStringDictionaryKeyCallBacks, 24, init_copystr_cb24);
GDATA(kCFTypeSetCallBacks, 24, init_type_cb24);
GDATA(kCFCopyStringSetCallBacks, 24, init_copystr_cb24);

// Custom guest callbacks: trampoline pool.
#define NCBSLOTS 16
static struct cbslot {
    gaddr_t gcb, retain, release, copydesc, equal, hash;
    CFDictionaryKeyCallBacks host; // superset layout of all callback structs
} cbslots[NCBSLOTS];
static int ncbslots;
static pthread_mutex_t cbslot_lock = PTHREAD_MUTEX_INITIALIZER;

static const void *slot_retain(int n, CFAllocatorRef a, const void *v) {
    return (const void *)(uintptr_t)gcall2(cbslots[n].retain, cfg(a), (uint32_t)(uintptr_t)v);
}
static void slot_release(int n, CFAllocatorRef a, const void *v) {
    gcall2(cbslots[n].release, cfg(a), (uint32_t)(uintptr_t)v);
}
static CFStringRef slot_copydesc(int n, const void *v) {
    return gcf_string(gcall1(cbslots[n].copydesc, (uint32_t)(uintptr_t)v));
}
static Boolean slot_equal(int n, const void *a, const void *b) {
    return (gcall2(cbslots[n].equal, (uint32_t)(uintptr_t)a, (uint32_t)(uintptr_t)b) & 0xff) != 0;
}
static CFHashCode slot_hash(int n, const void *v) { return gcall1(cbslots[n].hash, (uint32_t)(uintptr_t)v); }

#define SLOT_FNS(n)                                                                                \
    static const void *sret_##n(CFAllocatorRef a, const void *v) { return slot_retain(n, a, v); } \
    static void srel_##n(CFAllocatorRef a, const void *v) { slot_release(n, a, v); }              \
    static CFStringRef sdesc_##n(const void *v) { return slot_copydesc(n, v); }                   \
    static Boolean seq_##n(const void *a, const void *b) { return slot_equal(n, a, b); }          \
    static CFHashCode shash_##n(const void *v) { return slot_hash(n, v); }
SLOT_FNS(0) SLOT_FNS(1) SLOT_FNS(2) SLOT_FNS(3) SLOT_FNS(4) SLOT_FNS(5) SLOT_FNS(6) SLOT_FNS(7)
SLOT_FNS(8) SLOT_FNS(9) SLOT_FNS(10) SLOT_FNS(11) SLOT_FNS(12) SLOT_FNS(13) SLOT_FNS(14) SLOT_FNS(15)
#define SLOT_ENTRY(n) {sret_##n, srel_##n, sdesc_##n, seq_##n, shash_##n}
static const struct {
    CFDictionaryRetainCallBack retain;
    CFDictionaryReleaseCallBack release;
    CFDictionaryCopyDescriptionCallBack copydesc;
    CFDictionaryEqualCallBack equal;
    CFDictionaryHashCallBack hash;
} slot_fns[NCBSLOTS] = {SLOT_ENTRY(0),  SLOT_ENTRY(1),  SLOT_ENTRY(2),  SLOT_ENTRY(3), SLOT_ENTRY(4),  SLOT_ENTRY(5),
                        SLOT_ENTRY(6),  SLOT_ENTRY(7),  SLOT_ENTRY(8),  SLOT_ENTRY(9), SLOT_ENTRY(10), SLOT_ENTRY(11),
                        SLOT_ENTRY(12), SLOT_ENTRY(13), SLOT_ENTRY(14), SLOT_ENTRY(15)};

// Map a guest callback struct to host callbacks. *raw is set when elements are raw words.
static const void *map_callbacks(gaddr_t g, bool has_hash, bool copystr_ok, bool *raw, const void *cftype,
                                 const void *copystr) {
    *raw = false;
    if (!g) {
        *raw = true;
        return NULL;
    }
    pthread_once(&stubs_once, make_stubs);
    gaddr_t retain = gread32(g + 4), release = gread32(g + 8), desc = gread32(g + 12), equal = gread32(g + 16);
    gaddr_t hash = has_hash ? gread32(g + 20) : 0;
    if (retain == stub_retain && release == stub_release) return cftype;
    if (retain == stub_copystr && copystr_ok) return copystr;
    if (!retain && !release && !desc && !equal && !hash) {
        *raw = true;
        return NULL;
    }
    *raw = true;
    pthread_mutex_lock(&cbslot_lock);
    int n;
    for (n = 0; n < ncbslots; n++)
        if (cbslots[n].gcb == g && cbslots[n].retain == retain && cbslots[n].release == release &&
            cbslots[n].copydesc == desc && cbslots[n].equal == equal && cbslots[n].hash == hash)
            break;
    if (n == ncbslots) {
        if (ncbslots == NCBSLOTS) fatal("cf: too many distinct custom guest collection callback structs");
        struct cbslot *s = &cbslots[ncbslots++];
        s->gcb = g;
        s->retain = retain;
        s->release = release;
        s->copydesc = desc;
        s->equal = equal;
        s->hash = hash;
        s->host.version = 0;
        s->host.retain = retain ? slot_fns[n].retain : NULL;
        s->host.release = release ? slot_fns[n].release : NULL;
        s->host.copyDescription = desc ? slot_fns[n].copydesc : NULL;
        s->host.equal = equal ? slot_fns[n].equal : NULL;
        s->host.hash = hash ? slot_fns[n].hash : NULL;
        LOGI("cf: custom guest collection callbacks at %#x -> slot %d", g, n);
    }
    pthread_mutex_unlock(&cbslot_lock);
    return &cbslots[n].host;
}

static const CFArrayCallBacks *array_cb(gaddr_t g, bool *raw) {
    const void *p = map_callbacks(g, false, false, raw, &kCFTypeArrayCallBacks, NULL);
    // CFArrayCallBacks is a prefix-compatible layout ({version, retain, release, copyDescription, equal}).
    if (p && p != &kCFTypeArrayCallBacks) {
        static CFArrayCallBacks conv[NCBSLOTS];
        const CFDictionaryKeyCallBacks *k = p;
        int n = (int)(((const char *)p - (const char *)&cbslots[0].host) / sizeof cbslots[0]);
        conv[n] = (CFArrayCallBacks){0, k->retain, k->release, k->copyDescription, k->equal};
        return &conv[n];
    }
    return p;
}
static const CFDictionaryKeyCallBacks *key_cb(gaddr_t g, bool *raw) {
    return map_callbacks(g, true, true, raw, &kCFTypeDictionaryKeyCallBacks, &kCFCopyStringDictionaryKeyCallBacks);
}
static const CFDictionaryValueCallBacks *value_cb(gaddr_t g, bool *raw) {
    const void *p = map_callbacks(g, false, false, raw, &kCFTypeDictionaryValueCallBacks, NULL);
    if (p && p != &kCFTypeDictionaryValueCallBacks) {
        static CFDictionaryValueCallBacks conv[NCBSLOTS];
        const CFDictionaryKeyCallBacks *k = p;
        int n = (int)(((const char *)p - (const char *)&cbslots[0].host) / sizeof cbslots[0]);
        conv[n] = (CFDictionaryValueCallBacks){0, k->retain, k->release, k->copyDescription, k->equal};
        return &conv[n];
    }
    return p;
}
static const CFSetCallBacks *set_cb(gaddr_t g, bool *raw) {
    // CFSetCallBacks has the same layout as CFDictionaryKeyCallBacks.
    const void *p = map_callbacks(g, true, true, raw, &kCFTypeSetCallBacks, &kCFCopyStringSetCallBacks);
    return p;
}

// --- CFArray --------------------------------------------------------------------------------
#define A(i) ((CFArrayRef)ARGCF(i))
#define MA(i) ((CFMutableArrayRef)ARGCF(i))
static bool araw(CFTypeRef a) { return (cf_raw_flags(a) & CF_RAW_VALUES) != 0; }

THUNK(CFArrayGetTypeID) { RET(CFArrayGetTypeID()); }
THUNK(CFArrayCreate) {
    bool raw;
    const CFArrayCallBacks *cb = array_cb(ARG(3), &raw);
    CFIndex n = ARGIDX(2);
    const void **v = calloc(n > 0 ? n : 1, sizeof *v);
    for (CFIndex i = 0; i < n; i++) v[i] = cf_elem_from_guest(raw, gread32(ARG(1) + 4 * (uint32_t)i));
    CFArrayRef a = CFArrayCreate(gcf_allocator(ARG(0)), v, n, cb);
    free(v);
    if (raw) cf_set_raw_flags(a, CF_RAW_VALUES);
    RET_CF(a);
}
THUNK(CFArrayCreateMutable) {
    bool raw;
    const CFArrayCallBacks *cb = array_cb(ARG(2), &raw);
    CFMutableArrayRef a = CFArrayCreateMutable(gcf_allocator(ARG(0)), ARGIDX(1), cb);
    if (raw) cf_set_raw_flags(a, CF_RAW_VALUES);
    RET_CF(a);
}
THUNK(CFArrayCreateCopy) {
    CFArrayRef s = A(1);
    CFArrayRef a = CFArrayCreateCopy(gcf_allocator(ARG(0)), s);
    cf_set_raw_flags(a, cf_raw_flags(s));
    RET_CF(a);
}
THUNK(CFArrayCreateMutableCopy) {
    CFArrayRef s = A(2);
    CFMutableArrayRef a = CFArrayCreateMutableCopy(gcf_allocator(ARG(0)), ARGIDX(1), s);
    cf_set_raw_flags(a, cf_raw_flags(s));
    RET_CF(a);
}
THUNK(CFArrayGetCount) {
    CFArrayRef a = A(0);
    RET(a ? (uint32_t)CFArrayGetCount(a) : 0);
}
THUNK(CFArrayGetValueAtIndex) {
    CFArrayRef a = A(0);
    RET(cf_elem_to_guest(araw(a), CFArrayGetValueAtIndex(a, ARGIDX(1))));
}
THUNK(CFArrayGetValues) {
    CFArrayRef a = A(0);
    CFRange rg = ARGRANGE(1);
    if (rg.length <= 0) return;
    const void **v = calloc(rg.length, sizeof *v);
    CFArrayGetValues(a, rg, v);
    bool raw = araw(a);
    for (CFIndex i = 0; i < rg.length; i++) gwrite32(ARG(3) + 4 * (uint32_t)i, cf_elem_to_guest(raw, v[i]));
    free(v);
}
THUNK(CFArrayContainsValue) {
    CFArrayRef a = A(0);
    RET_BOOL(CFArrayContainsValue(a, ARGRANGE(1), cf_elem_from_guest(araw(a), ARG(3))));
}
THUNK(CFArrayGetFirstIndexOfValue) {
    CFArrayRef a = A(0);
    RET((uint32_t)CFArrayGetFirstIndexOfValue(a, ARGRANGE(1), cf_elem_from_guest(araw(a), ARG(3))));
}
THUNK(CFArrayGetLastIndexOfValue) {
    CFArrayRef a = A(0);
    RET((uint32_t)CFArrayGetLastIndexOfValue(a, ARGRANGE(1), cf_elem_from_guest(araw(a), ARG(3))));
}
THUNK(CFArrayGetCountOfValue) {
    CFArrayRef a = A(0);
    RET((uint32_t)CFArrayGetCountOfValue(a, ARGRANGE(1), cf_elem_from_guest(araw(a), ARG(3))));
}
THUNK(CFArrayAppendValue) {
    CFMutableArrayRef a = MA(0);
    CFArrayAppendValue(a, cf_elem_from_guest(araw(a), ARG(1)));
}
THUNK(CFArrayInsertValueAtIndex) {
    CFMutableArrayRef a = MA(0);
    CFArrayInsertValueAtIndex(a, ARGIDX(1), cf_elem_from_guest(araw(a), ARG(2)));
}
THUNK(CFArraySetValueAtIndex) {
    CFMutableArrayRef a = MA(0);
    CFArraySetValueAtIndex(a, ARGIDX(1), cf_elem_from_guest(araw(a), ARG(2)));
}
THUNK(CFArrayRemoveValueAtIndex) { CFArrayRemoveValueAtIndex(MA(0), ARGIDX(1)); }
THUNK(CFArrayRemoveAllValues) { CFArrayRemoveAllValues(MA(0)); }
THUNK(CFArrayExchangeValuesAtIndices) { CFArrayExchangeValuesAtIndices(MA(0), ARGIDX(1), ARGIDX(2)); }
THUNK(CFArrayAppendArray) { CFArrayAppendArray(MA(0), A(1), ARGRANGE(2)); }
THUNK(CFArrayReplaceValues) {
    CFMutableArrayRef a = MA(0);
    CFIndex n = ARGIDX(4);
    bool raw = araw(a);
    const void **v = calloc(n > 0 ? n : 1, sizeof *v);
    for (CFIndex i = 0; i < n; i++) v[i] = cf_elem_from_guest(raw, gread32(ARG(3) + 4 * (uint32_t)i));
    CFArrayReplaceValues(a, ARGRANGE(1), v, n);
    free(v);
}

struct gapply {
    gaddr_t fn, ctx;
    bool raw, rawkeys;
};
static void array_applier(const void *v, void *ctx) {
    struct gapply *a = ctx;
    gcall2(a->fn, cf_elem_to_guest(a->raw, v), a->ctx);
}
THUNK(CFArrayApplyFunction) {
    CFArrayRef a = A(0);
    struct gapply ap = {ARG(3), ARG(4), araw(a), false};
    CFArrayApplyFunction(a, ARGRANGE(1), array_applier, &ap);
}
static CFComparisonResult array_comparator(const void *x, const void *y, void *ctx) {
    struct gapply *a = ctx;
    return (CFComparisonResult)(int32_t)gcall3(a->fn, cf_elem_to_guest(a->raw, x), cf_elem_to_guest(a->raw, y), a->ctx);
}
THUNK(CFArraySortValues) {
    CFMutableArrayRef a = MA(0);
    struct gapply ap = {ARG(3), ARG(4), araw(a), false};
    CFArraySortValues(a, ARGRANGE(1), array_comparator, &ap);
}
THUNK(CFArrayBSearchValues) {
    CFArrayRef a = A(0);
    struct gapply ap = {ARG(4), ARG(5), araw(a), false};
    RET((uint32_t)CFArrayBSearchValues(a, ARGRANGE(1), cf_elem_from_guest(ap.raw, ARG(3)), array_comparator, &ap));
}

// --- CFDictionary ------------------------------------------------------------------------------
#define D(i) ((CFDictionaryRef)ARGCF(i))
#define MD(i) ((CFMutableDictionaryRef)ARGCF(i))
static bool kraw(CFTypeRef d) { return (cf_raw_flags(d) & CF_RAW_KEYS) != 0; }
static bool vraw(CFTypeRef d) { return (cf_raw_flags(d) & CF_RAW_VALUES) != 0; }

THUNK(CFDictionaryGetTypeID) { RET(CFDictionaryGetTypeID()); }
THUNK(CFDictionaryCreate) {
    // (alloc, keys*, values*, n, keyCB*, valueCB*)
    bool rk, rv;
    const CFDictionaryKeyCallBacks *kcb = key_cb(ARG(4), &rk);
    const CFDictionaryValueCallBacks *vcb = value_cb(ARG(5), &rv);
    CFIndex n = ARGIDX(3);
    const void **k = calloc(n > 0 ? n : 1, sizeof *k), **v = calloc(n > 0 ? n : 1, sizeof *v);
    for (CFIndex i = 0; i < n; i++) {
        k[i] = cf_elem_from_guest(rk, gread32(ARG(1) + 4 * (uint32_t)i));
        v[i] = cf_elem_from_guest(rv, gread32(ARG(2) + 4 * (uint32_t)i));
    }
    CFDictionaryRef d = CFDictionaryCreate(gcf_allocator(ARG(0)), k, v, n, kcb, vcb);
    free(k);
    free(v);
    cf_set_raw_flags(d, (rk ? CF_RAW_KEYS : 0) | (rv ? CF_RAW_VALUES : 0));
    RET_CF(d);
}
THUNK(CFDictionaryCreateMutable) {
    bool rk, rv;
    const CFDictionaryKeyCallBacks *kcb = key_cb(ARG(2), &rk);
    const CFDictionaryValueCallBacks *vcb = value_cb(ARG(3), &rv);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(gcf_allocator(ARG(0)), ARGIDX(1), kcb, vcb);
    cf_set_raw_flags(d, (rk ? CF_RAW_KEYS : 0) | (rv ? CF_RAW_VALUES : 0));
    RET_CF(d);
}
THUNK(CFDictionaryCreateCopy) {
    CFDictionaryRef s = D(1);
    CFDictionaryRef d = CFDictionaryCreateCopy(gcf_allocator(ARG(0)), s);
    cf_set_raw_flags(d, cf_raw_flags(s));
    RET_CF(d);
}
THUNK(CFDictionaryCreateMutableCopy) {
    CFDictionaryRef s = D(2);
    CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(gcf_allocator(ARG(0)), ARGIDX(1), s);
    cf_set_raw_flags(d, cf_raw_flags(s));
    RET_CF(d);
}
THUNK(CFDictionaryGetCount) {
    CFDictionaryRef d = D(0);
    RET(d ? (uint32_t)CFDictionaryGetCount(d) : 0);
}
THUNK(CFDictionaryGetCountOfKey) {
    CFDictionaryRef d = D(0);
    RET((uint32_t)CFDictionaryGetCountOfKey(d, cf_elem_from_guest(kraw(d), ARG(1))));
}
THUNK(CFDictionaryGetCountOfValue) {
    CFDictionaryRef d = D(0);
    RET((uint32_t)CFDictionaryGetCountOfValue(d, cf_elem_from_guest(vraw(d), ARG(1))));
}
THUNK(CFDictionaryContainsKey) {
    CFDictionaryRef d = D(0);
    RET_BOOL(d && CFDictionaryContainsKey(d, cf_elem_from_guest(kraw(d), ARG(1))));
}
THUNK(CFDictionaryContainsValue) {
    CFDictionaryRef d = D(0);
    RET_BOOL(d && CFDictionaryContainsValue(d, cf_elem_from_guest(vraw(d), ARG(1))));
}
THUNK(CFDictionaryGetValue) {
    CFDictionaryRef d = D(0);
    if (!d) { RET(0); return; }
    RET(cf_elem_to_guest(vraw(d), CFDictionaryGetValue(d, cf_elem_from_guest(kraw(d), ARG(1)))));
}
THUNK(CFDictionaryGetValueIfPresent) {
    CFDictionaryRef d = D(0);
    const void *v = NULL;
    Boolean ok = d && CFDictionaryGetValueIfPresent(d, cf_elem_from_guest(kraw(d), ARG(1)), &v);
    if (ok && ARG(2)) gwrite32(ARG(2), cf_elem_to_guest(vraw(d), v));
    RET_BOOL(ok);
}
THUNK(CFDictionaryGetKeysAndValues) {
    CFDictionaryRef d = D(0);
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc(n > 0 ? n : 1, sizeof *k), **v = calloc(n > 0 ? n : 1, sizeof *v);
    CFDictionaryGetKeysAndValues(d, k, v);
    bool rk = kraw(d), rv = vraw(d);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) gwrite32(ARG(1) + 4 * (uint32_t)i, cf_elem_to_guest(rk, k[i]));
        if (ARG(2)) gwrite32(ARG(2) + 4 * (uint32_t)i, cf_elem_to_guest(rv, v[i]));
    }
    free(k);
    free(v);
}
THUNK(CFDictionaryAddValue) {
    CFMutableDictionaryRef d = MD(0);
    CFDictionaryAddValue(d, cf_elem_from_guest(kraw(d), ARG(1)), cf_elem_from_guest(vraw(d), ARG(2)));
}
THUNK(CFDictionarySetValue) {
    CFMutableDictionaryRef d = MD(0);
    CFDictionarySetValue(d, cf_elem_from_guest(kraw(d), ARG(1)), cf_elem_from_guest(vraw(d), ARG(2)));
}
THUNK(CFDictionaryReplaceValue) {
    CFMutableDictionaryRef d = MD(0);
    CFDictionaryReplaceValue(d, cf_elem_from_guest(kraw(d), ARG(1)), cf_elem_from_guest(vraw(d), ARG(2)));
}
THUNK(CFDictionaryRemoveValue) {
    CFMutableDictionaryRef d = MD(0);
    CFDictionaryRemoveValue(d, cf_elem_from_guest(kraw(d), ARG(1)));
}
THUNK(CFDictionaryRemoveAllValues) { CFDictionaryRemoveAllValues(MD(0)); }
static void dict_applier(const void *k, const void *v, void *ctx) {
    struct gapply *a = ctx;
    gcall3(a->fn, cf_elem_to_guest(a->rawkeys, k), cf_elem_to_guest(a->raw, v), a->ctx);
}
THUNK(CFDictionaryApplyFunction) {
    CFDictionaryRef d = D(0);
    struct gapply ap = {ARG(1), ARG(2), vraw(d), kraw(d)};
    CFDictionaryApplyFunction(d, dict_applier, &ap);
}

// --- CFSet ------------------------------------------------------------------------------------------
#define SE(i) ((CFSetRef)ARGCF(i))
#define MSE(i) ((CFMutableSetRef)ARGCF(i))
THUNK(CFSetGetTypeID) { RET(CFSetGetTypeID()); }
THUNK(CFSetCreate) {
    bool raw;
    const CFSetCallBacks *cb = set_cb(ARG(3), &raw);
    CFIndex n = ARGIDX(2);
    const void **v = calloc(n > 0 ? n : 1, sizeof *v);
    for (CFIndex i = 0; i < n; i++) v[i] = cf_elem_from_guest(raw, gread32(ARG(1) + 4 * (uint32_t)i));
    CFSetRef s = CFSetCreate(gcf_allocator(ARG(0)), v, n, cb);
    free(v);
    if (raw) cf_set_raw_flags(s, CF_RAW_VALUES);
    RET_CF(s);
}
THUNK(CFSetCreateMutable) {
    bool raw;
    const CFSetCallBacks *cb = set_cb(ARG(2), &raw);
    CFMutableSetRef s = CFSetCreateMutable(gcf_allocator(ARG(0)), ARGIDX(1), cb);
    if (raw) cf_set_raw_flags(s, CF_RAW_VALUES);
    RET_CF(s);
}
THUNK(CFSetCreateCopy) {
    CFSetRef s = SE(1);
    CFSetRef c = CFSetCreateCopy(gcf_allocator(ARG(0)), s);
    cf_set_raw_flags(c, cf_raw_flags(s));
    RET_CF(c);
}
THUNK(CFSetCreateMutableCopy) {
    CFSetRef s = SE(2);
    CFMutableSetRef c = CFSetCreateMutableCopy(gcf_allocator(ARG(0)), ARGIDX(1), s);
    cf_set_raw_flags(c, cf_raw_flags(s));
    RET_CF(c);
}
THUNK(CFSetGetCount) {
    CFSetRef s = SE(0);
    RET(s ? (uint32_t)CFSetGetCount(s) : 0);
}
THUNK(CFSetContainsValue) {
    CFSetRef s = SE(0);
    RET_BOOL(s && CFSetContainsValue(s, cf_elem_from_guest(araw(s), ARG(1))));
}
THUNK(CFSetGetValue) {
    CFSetRef s = SE(0);
    RET(cf_elem_to_guest(araw(s), CFSetGetValue(s, cf_elem_from_guest(araw(s), ARG(1)))));
}
THUNK(CFSetGetValues) {
    CFSetRef s = SE(0);
    CFIndex n = CFSetGetCount(s);
    const void **v = calloc(n > 0 ? n : 1, sizeof *v);
    CFSetGetValues(s, v);
    bool raw = araw(s);
    for (CFIndex i = 0; i < n; i++) gwrite32(ARG(1) + 4 * (uint32_t)i, cf_elem_to_guest(raw, v[i]));
    free(v);
}
THUNK(CFSetAddValue) {
    CFMutableSetRef s = MSE(0);
    CFSetAddValue(s, cf_elem_from_guest(araw(s), ARG(1)));
}
THUNK(CFSetSetValue) {
    CFMutableSetRef s = MSE(0);
    CFSetSetValue(s, cf_elem_from_guest(araw(s), ARG(1)));
}
THUNK(CFSetRemoveValue) {
    CFMutableSetRef s = MSE(0);
    CFSetRemoveValue(s, cf_elem_from_guest(araw(s), ARG(1)));
}
THUNK(CFSetRemoveAllValues) { CFSetRemoveAllValues(MSE(0)); }
THUNK(CFSetApplyFunction) {
    CFSetRef s = SE(0);
    struct gapply ap = {ARG(1), ARG(2), araw(s), false};
    CFSetApplyFunction(s, array_applier, &ap);
}

// --- CFData ----------------------------------------------------------------------------------------
// Guest-created data objects use the guest heap allocator, so their bytes are
// guest memory and CFDataGetBytePtr/GetMutableBytePtr can return them directly.
#define DA(i) ((CFDataRef)ARGCF(i))
#define MDA(i) ((CFMutableDataRef)ARGCF(i))
static CFAllocatorRef data_alloc(gaddr_t g) {
    CFAllocatorRef a = gcf_allocator(g);
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc) return cf_guest_heap_allocator();
    return a;
}
static char byteptr_key;

THUNK(CFDataGetTypeID) { RET(CFDataGetTypeID()); }
THUNK(CFDataCreate) { RET_CF(CFDataCreate(data_alloc(ARG(0)), ARGP(1), ARGIDX(2))); }
THUNK(CFDataCreateWithBytesNoCopy) {
    RET_CF(CFDataCreateWithBytesNoCopy(data_alloc(ARG(0)), ARGP(1), ARGIDX(2), cf_guest_deallocator(ARG(3))));
}
THUNK(CFDataCreateCopy) { RET_CF(CFDataCreateCopy(data_alloc(ARG(0)), DA(1))); }
THUNK(CFDataCreateMutable) { RET_CF(CFDataCreateMutable(data_alloc(ARG(0)), ARGIDX(1))); }
THUNK(CFDataCreateMutableCopy) { RET_CF(CFDataCreateMutableCopy(data_alloc(ARG(0)), ARGIDX(1), DA(2))); }
THUNK(CFDataGetLength) {
    CFDataRef d = DA(0);
    RET(d ? (uint32_t)CFDataGetLength(d) : 0);
}
THUNK(CFDataGetBytePtr) {
    CFDataRef d = DA(0);
    if (!d) { RET(0); return; }
    const UInt8 *p = CFDataGetBytePtr(d);
    if (!p) { RET(0); return; }
    if (host_ptr_in_guest(p)) { RET(h2g(p)); return; }
    RET(cf_guest_copy(d, &byteptr_key, p, (uint32_t)CFDataGetLength(d)));
}
THUNK(CFDataGetMutableBytePtr) {
    CFMutableDataRef d = MDA(0);
    if (!d) { RET(0); return; }
    UInt8 *p = CFDataGetMutableBytePtr(d);
    if (!p) { RET(0); return; }
    if (host_ptr_in_guest(p)) { RET(h2g(p)); return; }
    LOGE("CFDataGetMutableBytePtr: data %p has host storage %p; guest writes will not be seen", d, p);
    RET(cf_guest_copy(d, &byteptr_key, p, (uint32_t)CFDataGetLength(d)));
}
THUNK(CFDataGetBytes) { CFDataGetBytes(DA(0), ARGRANGE(1), ARGP(3)); }
THUNK(CFDataSetLength) { CFDataSetLength(MDA(0), ARGIDX(1)); }
THUNK(CFDataIncreaseLength) { CFDataIncreaseLength(MDA(0), ARGIDX(1)); }
THUNK(CFDataAppendBytes) { CFDataAppendBytes(MDA(0), ARGP(1), ARGIDX(2)); }
THUNK(CFDataReplaceBytes) { CFDataReplaceBytes(MDA(0), ARGRANGE(1), ARGP(3), ARGIDX(4)); }
THUNK(CFDataDeleteBytes) { CFDataDeleteBytes(MDA(0), ARGRANGE(1)); }

// --- CFTree -------------------------------------------------------------------------------------------
// CFTreeContext (i386) = {version, info, retain, release, copyDescription}.
// Guest-created trees get a host context whose info is a struct gtreeinfo;
// trees created by the host (CFXMLTree) carry CF objects as info.
#define T(i) ((CFTreeRef)ARGCF(i))
struct gtreeinfo {
    uint32_t magic;
    struct gcf_context ctx;
};
#define GTREE_MAGIC 0x47545245u
static const void *gtree_retain(const void *info) { return info; }
static void gtree_release(const void *info) {
    struct gtreeinfo *t = (struct gtreeinfo *)info;
    gcf_context_release(&t->ctx);
    t->magic = 0;
    free(t);
}
static char gtree_key;
THUNK(CFTreeGetTypeID) { RET(CFTreeGetTypeID()); }
THUNK(CFTreeCreate) {
    struct gtreeinfo *t = calloc(1, sizeof *t);
    t->magic = GTREE_MAGIC;
    gcf_context_read(ARG(1), &t->ctx, true);
    CFTreeContext c = {0, t, gtree_retain, gtree_release, NULL};
    CFTreeRef tree = CFTreeCreate(gcf_allocator(ARG(0)), &c);
    cf_attach(tree, &gtree_key, (void *)1, NULL);
    RET_CF(tree);
}
THUNK(CFTreeGetContext) {
    CFTreeRef tree = T(0);
    CFTreeContext c = {0};
    CFTreeGetContext(tree, &c);
    struct gcf_context g = {0};
    if (cf_attached(tree, &gtree_key)) g = ((struct gtreeinfo *)c.info)->ctx;
    else g.info = cfg(c.info); // host trees (e.g. CFXMLTree): info is a CF object
    gcf_context_write(ARG(1), &g);
}
THUNK(CFTreeSetContext) {
    CFTreeRef tree = T(0);
    struct gtreeinfo *t = calloc(1, sizeof *t);
    t->magic = GTREE_MAGIC;
    gcf_context_read(ARG(1), &t->ctx, true);
    CFTreeContext c = {0, t, gtree_retain, gtree_release, NULL};
    CFTreeSetContext(tree, &c);
    cf_attach(tree, &gtree_key, (void *)1, NULL);
}
THUNK(CFTreeGetParent) { RET_CF(CFTreeGetParent(T(0))); }
THUNK(CFTreeGetNextSibling) { RET_CF(CFTreeGetNextSibling(T(0))); }
THUNK(CFTreeGetFirstChild) { RET_CF(CFTreeGetFirstChild(T(0))); }
THUNK(CFTreeGetChildCount) { RET((uint32_t)CFTreeGetChildCount(T(0))); }
THUNK(CFTreeGetChildAtIndex) { RET_CF(CFTreeGetChildAtIndex(T(0), ARGIDX(1))); }
THUNK(CFTreeFindRoot) { RET_CF(CFTreeFindRoot(T(0))); }
THUNK(CFTreeGetChildren) {
    CFTreeRef t = T(0);
    CFIndex n = CFTreeGetChildCount(t);
    CFTreeRef *c = calloc(n > 0 ? n : 1, sizeof *c);
    CFTreeGetChildren(t, c);
    for (CFIndex i = 0; i < n; i++) gwrite32(ARG(1) + 4 * (uint32_t)i, cfg(c[i]));
    free(c);
}
THUNK(CFTreeAppendChild) { CFTreeAppendChild(T(0), T(1)); }
THUNK(CFTreePrependChild) { CFTreePrependChild(T(0), T(1)); }
THUNK(CFTreeInsertSibling) { CFTreeInsertSibling(T(0), T(1)); }
THUNK(CFTreeRemove) { CFTreeRemove(T(0)); }
THUNK(CFTreeRemoveAllChildren) { CFTreeRemoveAllChildren(T(0)); }
static void tree_applier(const void *v, void *ctx) {
    struct gapply *a = ctx;
    gcall2(a->fn, cfg(v), a->ctx);
}
THUNK(CFTreeApplyFunctionToChildren) {
    struct gapply ap = {ARG(1), ARG(2), false, false};
    CFTreeApplyFunctionToChildren(T(0), tree_applier, &ap);
}
