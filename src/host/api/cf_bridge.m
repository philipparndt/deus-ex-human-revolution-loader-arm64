// CoreFoundation bridge core: handle mapping, constant CFString literals,
// lifetime-bound attachments, guest allocators and CF-style formatting.
#include "cf_bridge.h"

#include "../runtime.h"

#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <pthread.h>

// --- constant string literals -------------------------------------------------------

static void init_cfconst_class(gaddr_t a) { (void)a; }
GDATA(__CFConstantStringClassReference, 32, init_cfconst_class);
static gaddr_t cfconst_class_addr;

gaddr_t cf_constant_string_class(void) {
    if (!cfconst_class_addr) cfconst_class_addr = hostsym_lookup("___CFConstantStringClassReference");
    return cfconst_class_addr;
}

static pthread_mutex_t const_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef const_g2h; // (void*)gaddr -> CFStringRef
static CFMutableDictionaryRef const_h2g; // CFStringRef -> (void*)gaddr

static cf_foreign_resolver foreign_resolver;
static cf_wrap_hook wrap_hook;
void cf_bridge_set_foreign_resolver(cf_foreign_resolver fn) { foreign_resolver = fn; }
void cf_bridge_set_wrap_hook(cf_wrap_hook fn) { wrap_hook = fn; }

bool cf_is_guest_constant(gaddr_t g) {
    if (!g || hobj_is_handle(g) || (g & 3) || !gmem_is_mapped(g, 16)) return false;
    return gread32(g) == cf_constant_string_class();
}

static CFStringRef make_constant(gaddr_t g) {
    uint32_t flags = gread32(g + 4), chars = gread32(g + 8);
    int32_t len = (int32_t)gread32(g + 12);
    CFStringRef s;
    if (len <= 0 || !chars)
        s = CFStringCreateWithBytes(NULL, (const UInt8 *)"", 0, kCFStringEncodingASCII, false);
    else if (flags & 0x10) // __kCFIsUnicode
        s = CFStringCreateWithCharacters(NULL, (const UniChar *)g2h(chars), len);
    else {
        s = CFStringCreateWithBytes(NULL, (const UInt8 *)g2h(chars), len, kCFStringEncodingUTF8, false);
        if (!s) s = CFStringCreateWithBytes(NULL, (const UInt8 *)g2h(chars), len, kCFStringEncodingMacRoman, false);
    }
    return s;
}

static CFTypeRef lookup_constant(gaddr_t g) {
    pthread_mutex_lock(&const_lock);
    if (!const_g2h) {
        const_g2h = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
        const_h2g = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    }
    CFStringRef s = CFDictionaryGetValue(const_g2h, (void *)(uintptr_t)g);
    if (!s) {
        s = make_constant(g);
        if (s) {
            CFRetain(s); // immortal (one extra reference that is never dropped)
            CFDictionarySetValue(const_g2h, (void *)(uintptr_t)g, s);
            if (!CFDictionaryContainsKey(const_h2g, s)) CFDictionarySetValue(const_h2g, s, (void *)(uintptr_t)g);
        }
    }
    pthread_mutex_unlock(&const_lock);
    return s;
}

static bool constant_for_host(CFTypeRef h, gaddr_t *g) {
    if (!const_h2g) return false;
    pthread_mutex_lock(&const_lock);
    const void *v = NULL;
    bool found = CFDictionaryGetValueIfPresent(const_h2g, h, &v);
    pthread_mutex_unlock(&const_lock);
    if (found) *g = (gaddr_t)(uintptr_t)v;
    return found;
}

// --- handles ------------------------------------------------------------------------------

CFTypeRef gcf(gaddr_t g) {
    if (!g) return NULL;
    if (hobj_is_handle(g)) {
        void *h = hobj_unwrap(g);
        if (!h) LOGE("cf: stale or invalid handle %#x", g);
        return h;
    }
    if (cf_is_guest_constant(g)) return lookup_constant(g);
    if (foreign_resolver) {
        CFTypeRef h = foreign_resolver(g);
        if (h) return h;
    }
    char where[128];
    LOGE("cf: guest address %#x is not a CF object handle (%s, heap size %u)", g,
         macho_describe_addr(g, where, sizeof where), gheap_size(g));
    return NULL;
}

gaddr_t cfg(CFTypeRef h) {
    if (!h) return 0;
    gaddr_t g;
    if (constant_for_host(h, &g)) return g;
    g = hobj_wrap(h);
    if (wrap_hook && gread32(g) == 0) wrap_hook(g, h);
    return g;
}

// --- strings -----------------------------------------------------------------------------

char *cf_utf8(CFStringRef s) {
    if (!s) return NULL;
    CFIndex len = CFStringGetLength(s);
    CFIndex cap = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char *buf = malloc(cap);
    if (!CFStringGetCString(s, buf, cap, kCFStringEncodingUTF8)) buf[0] = 0;
    return buf;
}

gaddr_t cf_guest_utf8(CFStringRef s) {
    if (!s) return 0;
    char *u = cf_utf8(s);
    gaddr_t g = gheap_strdup(u);
    free(u);
    return g;
}

CFStringRef cf_string_from_guest(gaddr_t s) {
    if (!s) return NULL;
    const char *p = g2h(s);
    CFStringRef r = CFStringCreateWithCString(NULL, p, kCFStringEncodingUTF8);
    if (!r) r = CFStringCreateWithCString(NULL, p, kCFStringEncodingMacRoman);
    return r;
}

// --- attachments ----------------------------------------------------------------------------

@interface DXHAttachment : NSObject {
  @public
    void *ptr;
    void (*dtor)(void *);
    gaddr_t gbuf;
    uint32_t gcap;
}
@end
@implementation DXHAttachment
- (void)dealloc {
    if (dtor) dtor(ptr);
    if (gbuf) gheap_free(gbuf);
}
@end

static bool is_tagged(CFTypeRef p) {
#if defined(__x86_64__)
    return ((uintptr_t)p & 1) != 0;
#else
    return (intptr_t)p < 0;
#endif
}

// Tagged pointers cannot carry associated objects; they are immutable values,
// so a global (never freed) table keyed by (value, key) is correct for them.
static pthread_mutex_t tagged_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary *tagged_attachments;

static DXHAttachment *attachment(CFTypeRef obj, const void *key, bool create) {
    if (!obj) return nil;
    if (is_tagged(obj)) {
        pthread_mutex_lock(&tagged_lock);
        if (!tagged_attachments) tagged_attachments = [NSMutableDictionary new];
        NSString *k = [NSString stringWithFormat:@"%p/%p", obj, key];
        DXHAttachment *a = tagged_attachments[k];
        if (!a && create) {
            a = [DXHAttachment new];
            tagged_attachments[k] = a;
        }
        pthread_mutex_unlock(&tagged_lock);
        return a;
    }
    id o = (__bridge id)obj;
    DXHAttachment *a = objc_getAssociatedObject(o, key);
    if (!a && create) {
        a = [DXHAttachment new];
        objc_setAssociatedObject(o, key, a, OBJC_ASSOCIATION_RETAIN);
    }
    return a;
}

void cf_attach(CFTypeRef obj, const void *key, void *ptr, void (*dtor)(void *)) {
    DXHAttachment *a = attachment(obj, key, true);
    if (!a) return;
    @synchronized(a) {
        if (a->dtor && a->ptr != ptr) a->dtor(a->ptr);
        a->ptr = ptr;
        a->dtor = dtor;
    }
}

void *cf_attached(CFTypeRef obj, const void *key) {
    DXHAttachment *a = attachment(obj, key, false);
    return a ? a->ptr : NULL;
}

gaddr_t cf_guest_copy(CFTypeRef obj, const void *key, const void *data, uint32_t size) {
    DXHAttachment *a = attachment(obj, key, true);
    if (!a) return 0;
    gaddr_t g;
    @synchronized(a) {
        if (!a->gbuf || a->gcap < size + 1) {
            if (a->gbuf) gheap_free(a->gbuf);
            a->gcap = size + 1 < 16 ? 16 : size + 1;
            a->gbuf = gheap_alloc(a->gcap);
            if (size > 4096) LOGD("cf: guest copy of %u bytes for %p", size, obj);
        }
        if (size) memcpy(g2h(a->gbuf), data, size);
        gwrite8(a->gbuf + size, 0);
        g = a->gbuf;
    }
    return g;
}

// --- raw containers ---------------------------------------------------------------------------

static char raw_flags_key;
void cf_set_raw_flags(CFTypeRef container, unsigned flags) {
    if (flags) cf_attach(container, &raw_flags_key, (void *)(uintptr_t)flags, NULL);
}
unsigned cf_raw_flags(CFTypeRef container) {
    if (!container || is_tagged(container)) return 0;
    return (unsigned)(uintptr_t)cf_attached(container, &raw_flags_key);
}

// --- guest context structs -----------------------------------------------------------------------

void gcf_context_read(gaddr_t ctx, struct gcf_context *out, bool call_retain) {
    memset(out, 0, sizeof *out);
    if (!ctx) return;
    out->version = gread32(ctx);
    out->info = gread32(ctx + 4);
    out->retain = gread32(ctx + 8);
    out->release = gread32(ctx + 12);
    out->copydesc = gread32(ctx + 16);
    if (call_retain && out->retain) {
        uint32_t r = gcall1(out->retain, out->info);
        if (r) out->info = r;
    }
}

void gcf_context_release(const struct gcf_context *c) {
    if (c->release) gcall1(c->release, c->info);
}

void gcf_context_write(gaddr_t ctx, const struct gcf_context *c) {
    if (!ctx) return;
    gwrite32(ctx, c->version);
    gwrite32(ctx + 4, c->info);
    gwrite32(ctx + 8, c->retain);
    gwrite32(ctx + 12, c->release);
    gwrite32(ctx + 16, c->copydesc);
}

// --- guest allocators ------------------------------------------------------------------------------
// CFAllocatorContext (i386): version, info, retain, release, copyDescription,
// allocate, reallocate, deallocate, preferredSize -- all 32-bit.

struct gallocator {
    gaddr_t info, retain, release, copydesc, allocate, reallocate, deallocate, preferred;
};

static void *galloc_allocate(CFIndex size, CFOptionFlags hint, void *info) {
    struct gallocator *a = info;
    if (!a->allocate) return NULL;
    return g2h(gcall3(a->allocate, (uint32_t)size, (uint32_t)hint, a->info));
}
static void *galloc_reallocate(void *ptr, CFIndex size, CFOptionFlags hint, void *info) {
    struct gallocator *a = info;
    if (!a->reallocate) return NULL;
    uint32_t args[4] = {h2g(ptr), (uint32_t)size, (uint32_t)hint, a->info};
    return g2h((gaddr_t)cpu_call(a->reallocate, 4, args));
}
static void galloc_deallocate(void *ptr, void *info) {
    struct gallocator *a = info;
    if (a->deallocate) gcall2(a->deallocate, h2g(ptr), a->info);
}
static CFIndex galloc_preferred(CFIndex size, CFOptionFlags hint, void *info) {
    struct gallocator *a = info;
    if (!a->preferred) return size;
    return (int32_t)gcall3(a->preferred, (uint32_t)size, (uint32_t)hint, a->info);
}
static void galloc_release(const void *info) {
    struct gallocator *a = (struct gallocator *)info;
    if (a->release) gcall1(a->release, a->info);
    free(a);
}

THUNK(CFAllocatorCreate) {
    gaddr_t ctx = ARG(1);
    if (!ctx) { RET(0); return; }
    CFAllocatorRef base = gcf_allocator(ARG(0));
    if (ARG(0) == hostsym_lookup("_kCFAllocatorUseContext") && ARG(0)) base = kCFAllocatorUseContext;
    struct gallocator *a = calloc(1, sizeof *a);
    a->info = gread32(ctx + 4);
    a->retain = gread32(ctx + 8);
    a->release = gread32(ctx + 12);
    a->copydesc = gread32(ctx + 16);
    a->allocate = gread32(ctx + 20);
    a->reallocate = gread32(ctx + 24);
    a->deallocate = gread32(ctx + 28);
    a->preferred = gread32(ctx + 32);
    if (a->retain) a->info = gcall1(a->retain, a->info);
    CFAllocatorContext hc = {0, a, NULL, galloc_release, NULL, galloc_allocate,
                             a->reallocate ? galloc_reallocate : NULL, a->deallocate ? galloc_deallocate : NULL,
                             a->preferred ? galloc_preferred : NULL};
    CFAllocatorRef h = CFAllocatorCreate(base, &hc);
    RET_CF(h);
}

CFAllocatorRef gcf_allocator(gaddr_t g) {
    if (!g) return NULL;
    return (CFAllocatorRef)gcf(g);
}

static void *gheap_cf_allocate(CFIndex size, CFOptionFlags hint, void *info) {
    return g2h(gheap_alloc((uint32_t)size));
}
static void *gheap_cf_reallocate(void *ptr, CFIndex size, CFOptionFlags hint, void *info) {
    return g2h(gheap_realloc(h2g(ptr), (uint32_t)size));
}
static void gheap_cf_deallocate(void *ptr, void *info) { gheap_free(h2g(ptr)); }
static CFIndex gheap_cf_preferred(CFIndex size, CFOptionFlags hint, void *info) { return (size + 15) & ~15; }

CFAllocatorRef cf_guest_heap_allocator(void) {
    static CFAllocatorRef a;
    if (!a) {
        CFAllocatorContext c = {0, NULL, NULL, NULL, NULL, gheap_cf_allocate, gheap_cf_reallocate,
                                gheap_cf_deallocate, gheap_cf_preferred};
        CFAllocatorRef n = CFAllocatorCreate(kCFAllocatorUseContext, &c);
        if (!__sync_bool_compare_and_swap(&a, NULL, n)) CFRelease(n);
    }
    return a;
}

CFAllocatorRef cf_guest_deallocator(gaddr_t g) {
    CFAllocatorRef a = gcf_allocator(g);
    if (a == kCFAllocatorNull) return kCFAllocatorNull;
    if (!a || a == kCFAllocatorSystemDefault || a == kCFAllocatorMalloc || a == kCFAllocatorDefault)
        return cf_guest_heap_allocator();
    return a;
}

// --- formatting ---------------------------------------------------------------------------------

struct fbuf {
    char *p;
    size_t len, cap;
};
static void fb_put(struct fbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void fb_puts(struct fbuf *b, const char *s) { fb_put(b, s, strlen(s)); }

enum { A_NONE, A_I32, A_I64, A_DBL, A_LDBL };
static int arg_size(int t) { return t == A_I64 || t == A_DBL ? 8 : t == A_LDBL ? 16 : 4; }

struct fspec {
    const char *start, *end; // whole spec including '%'
    char flags[8];
    int width, width_arg;    // width_arg: arg index for '*', -1 otherwise
    int prec, prec_arg;      // prec -1 = none
    char len[3];
    char conv;
    int arg;                 // value argument index (-1 for %%)
};

#define MAX_SPECS 128
#define MAX_ARGS 128

static void put_utf16(struct fbuf *b, const UniChar *s, CFIndex n) {
    CFStringRef t = CFStringCreateWithCharacters(NULL, s, n);
    if (!t) return;
    char *u = cf_utf8(t);
    fb_puts(b, u);
    free(u);
    CFRelease(t);
}

CFStringRef cf_format_guest(CFStringRef fmtstr, struct gva va) {
    if (!fmtstr) return NULL;
    char *fmt = cf_utf8(fmtstr);
    struct fspec specs[MAX_SPECS];
    int nspecs = 0, next_arg = 0, types[MAX_ARGS] = {0}, maxarg = -1;
    // Pass 1: parse specs and argument types.
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (nspecs >= MAX_SPECS) break;
        struct fspec *s = &specs[nspecs++];
        memset(s, 0, sizeof *s);
        s->start = p++;
        s->width_arg = s->prec_arg = -1;
        s->prec = -1;
        // positional n$
        int pos = -1;
        const char *q = p;
        int n = 0;
        while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
        if (*q == '$' && q > p) {
            pos = n - 1;
            p = q + 1;
        }
        int nf = 0;
        while (*p && strchr("-+ #0'", *p)) {
            if (nf < 7 && *p != '\'') s->flags[nf++] = *p;
            p++;
        }
        if (*p == '*') {
            p++;
            n = 0;
            q = p;
            while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            if (*q == '$' && q > p) {
                s->width_arg = n - 1;
                p = q + 1;
            } else
                s->width_arg = next_arg++;
            if (s->width_arg < MAX_ARGS) types[s->width_arg] = A_I32;
        } else
            while (*p >= '0' && *p <= '9') s->width = s->width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++;
            s->prec = 0;
            if (*p == '*') {
                p++;
                n = 0;
                q = p;
                while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
                if (*q == '$' && q > p) {
                    s->prec_arg = n - 1;
                    p = q + 1;
                } else
                    s->prec_arg = next_arg++;
                if (s->prec_arg < MAX_ARGS) types[s->prec_arg] = A_I32;
            } else
                while (*p >= '0' && *p <= '9') s->prec = s->prec * 10 + (*p++ - '0');
        }
        int nl = 0;
        while (*p && strchr("hlqLztj", *p)) {
            if (nl < 2) s->len[nl++] = *p;
            p++;
        }
        s->conv = *p;
        s->end = *p ? p + 1 : p;
        if (s->conv == '%' || !s->conv) {
            s->arg = -1;
        } else {
            s->arg = pos >= 0 ? pos : next_arg++;
            int t = A_I32;
            bool ll = (s->len[0] == 'l' && s->len[1] == 'l') || s->len[0] == 'q' || s->len[0] == 'j';
            if (strchr("fFeEgGaA", s->conv))
                t = s->len[0] == 'L' ? A_LDBL : A_DBL;
            else if (strchr("diouxXDOU", s->conv) && ll)
                t = A_I64;
            if (s->arg < MAX_ARGS) types[s->arg] = t;
        }
        if (s->arg > maxarg) maxarg = s->arg;
        if (s->width_arg > maxarg) maxarg = s->width_arg;
        if (s->prec_arg > maxarg) maxarg = s->prec_arg;
        if (!*p) break;
    }
    // Argument offsets.
    uint32_t offs[MAX_ARGS];
    uint32_t off = 0;
    for (int i = 0; i <= maxarg && i < MAX_ARGS; i++) {
        offs[i] = off;
        off += arg_size(types[i] ? types[i] : A_I32);
    }
    // Pass 2: output.
    struct fbuf b = {0};
    fb_put(&b, "", 0);
    const char *lit = fmt;
    for (int i = 0; i < nspecs; i++) {
        struct fspec *s = &specs[i];
        fb_put(&b, lit, (size_t)(s->start - lit));
        lit = s->end;
        if (s->arg < 0) {
            if (s->conv == '%') fb_put(&b, "%", 1);
            continue;
        }
        if (s->arg >= MAX_ARGS) continue;
        gaddr_t ap = va.p + offs[s->arg];
        int width = s->width_arg >= 0 && s->width_arg < MAX_ARGS ? (int32_t)gread32(va.p + offs[s->width_arg]) : s->width;
        int prec = s->prec_arg >= 0 && s->prec_arg < MAX_ARGS ? (int32_t)gread32(va.p + offs[s->prec_arg]) : s->prec;
        char spec[48], tmp[512];
        char *out = NULL;
        // Strings and objects: produce text, then pad with %*.*s semantics.
        const char *text = NULL;
        char *owned = NULL;
        switch (s->conv) {
        case '@': {
            CFTypeRef o = gcf(gread32(ap));
            if (!o)
                text = "(null)";
            else if (CFGetTypeID(o) == CFStringGetTypeID())
                text = owned = cf_utf8((CFStringRef)o);
            else {
                CFStringRef d = CFCopyDescription(o);
                text = owned = cf_utf8(d);
                if (d) CFRelease(d);
            }
            break;
        }
        case 's': {
            gaddr_t sp = gread32(ap);
            text = sp ? (const char *)g2h(sp) : "(null)";
            if (s->len[0] == 'l') { // %ls: wchar_t (32-bit)
                struct fbuf w = {0};
                fb_put(&w, "", 0);
                for (gaddr_t c = sp; c && gread32(c); c += 4) {
                    UniChar u[2];
                    UTF32Char ch = gread32(c);
                    if (ch >= 0x10000) {
                        ch -= 0x10000;
                        u[0] = 0xD800 + (ch >> 10);
                        u[1] = 0xDC00 + (ch & 0x3FF);
                        put_utf16(&w, u, 2);
                    } else {
                        u[0] = (UniChar)ch;
                        put_utf16(&w, u, 1);
                    }
                }
                text = owned = w.p;
            }
            break;
        }
        case 'S': {
            gaddr_t sp = gread32(ap);
            struct fbuf w = {0};
            fb_put(&w, "", 0);
            if (!sp) fb_puts(&w, "(null)");
            else {
                CFIndex n = 0;
                while (gread16(sp + 2 * (uint32_t)n)) n++;
                put_utf16(&w, (const UniChar *)g2h(sp), n);
            }
            text = owned = w.p;
            break;
        }
        case 'C': {
            UniChar u = (UniChar)gread32(ap);
            struct fbuf w = {0};
            fb_put(&w, "", 0);
            put_utf16(&w, &u, 1);
            text = owned = w.p;
            break;
        }
        case 'n': continue;
        default: break;
        }
        if (text) {
            // Precision counts bytes here (CF counts characters); good enough for padding.
            snprintf(spec, sizeof spec, "%%%s%d.%ds", strchr(s->flags, '-') ? "-" : "", width,
                     prec >= 0 ? prec : INT32_MAX / 2);
            int n = asprintf(&out, spec, text);
            if (n >= 0) fb_put(&b, out, (size_t)n);
            free(out);
            free(owned);
            continue;
        }
        // Numeric conversions.
        int t = types[s->arg];
        char conv = s->conv;
        const char *lenmod = "";
        if (conv == 'D' || conv == 'O' || conv == 'U') conv = (char)(conv + ('a' - 'A')), lenmod = "";
        if (t == A_I64) lenmod = "ll";
        else if (s->len[0] == 'h' && s->len[1] == 'h') lenmod = "hh";
        else if (s->len[0] == 'h') lenmod = "h";
        int n;
        if (prec >= 0)
            snprintf(spec, sizeof spec, "%%%s%d.%d%s%c", s->flags, width, prec, lenmod, conv);
        else
            snprintf(spec, sizeof spec, "%%%s%d%s%c", s->flags, width, lenmod, conv);
        if (conv == 'p') {
            n = snprintf(tmp, sizeof tmp, "0x%x", gread32(ap));
        } else if (t == A_DBL || t == A_LDBL) {
            double d;
            if (t == A_LDBL) d = x87_to_double(g2h(ap));
            else {
                uint64_t v = gread32(ap) | ((uint64_t)gread32(ap + 4) << 32);
                memcpy(&d, &v, 8);
            }
            n = snprintf(tmp, sizeof tmp, spec, d);
        } else if (t == A_I64) {
            uint64_t v = gread32(ap) | ((uint64_t)gread32(ap + 4) << 32);
            n = snprintf(tmp, sizeof tmp, spec, (long long)v);
        } else if (conv == 'c') {
            n = snprintf(tmp, sizeof tmp, spec, (int)(int8_t)gread32(ap));
        } else {
            uint32_t v = gread32(ap);
            if (strchr("di", conv)) n = snprintf(tmp, sizeof tmp, spec, (int)(int32_t)v);
            else n = snprintf(tmp, sizeof tmp, spec, (unsigned)v);
        }
        if (n > 0) fb_put(&b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
    fb_puts(&b, lit);
    CFStringRef r = CFStringCreateWithBytes(NULL, (const UInt8 *)b.p, (CFIndex)b.len, kCFStringEncodingUTF8, false);
    if (!r) r = CFStringCreateWithBytes(NULL, (const UInt8 *)b.p, (CFIndex)b.len, kCFStringEncodingMacRoman, false);
    free(b.p);
    free(fmt);
    return r;
}

// --- main bundle ------------------------------------------------------------------------------------

CFBundleRef cf_main_bundle(void) {
    static CFBundleRef bundle;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!bundle) {
        struct image *img = runtime_image(0);
        const char *path = img ? img->path : NULL;
        const char *app = path ? strstr(path, ".app/") : NULL;
        if (app) {
            CFURLRef url =
                CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, app + 4 - path, true);
            bundle = CFBundleCreate(NULL, url);
            CFRelease(url);
            if (bundle) LOGI("cf: main bundle is %.*s", (int)(app + 4 - path), path);
        }
        if (!bundle) {
            bundle = CFBundleGetMainBundle();
            CFRetain(bundle);
        }
    }
    pthread_mutex_unlock(&lock);
    return bundle;
}
