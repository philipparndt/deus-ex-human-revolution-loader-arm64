// Argument/return marshalling between i386 guest calls and host methods.
//
// Each side is described by its own ObjC type encoding: the guest by the i386
// encoding (from the generated SDK signature table or the guest's own method
// lists), the host by the runtime's encoding. Values are converted pairwise
// (CGFloat float<->double, NSInteger 32<->64 bit, objects <-> handles, geometry
// structs, out-parameters). Host methods are invoked through libffi.
#import "objc_internal.h"

#import <Foundation/Foundation.h>
#include <ffi/ffi.h>
#include <pthread.h>

enum abi { ABI_GUEST, ABI_HOST };

struct ty {
    char code;      // c C s S i I l L q Q f d D B v * @ # : ^ { ( [ ? b
    bool is_block;  // "@?"
    uint32_t size, align;
    int nfields;
    struct ty *fields; // struct/union members, array element (1)
    uint32_t *offsets;
    uint32_t count; // array length
    struct ty *pointee;
    char name[48];
};

static const char *skip_qualifiers(const char *p) {
    while (*p && strchr("rnNoORVAj", *p)) p++;
    return p;
}
static const char *skip_digits(const char *p) {
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

static const char *parse_ty(const char *p, struct ty *t, enum abi abi);

static void scalar(struct ty *t, uint32_t gs, uint32_t ga, uint32_t hs, uint32_t ha, enum abi abi) {
    t->size = abi == ABI_GUEST ? gs : hs;
    t->align = abi == ABI_GUEST ? ga : ha;
}

static const char *parse_aggregate(const char *p, struct ty *t, enum abi abi, char close) {
    // name
    const char *start = p;
    while (*p && *p != '=' && *p != close) p++;
    size_t nl = (size_t)(p - start);
    if (nl >= sizeof t->name) nl = sizeof t->name - 1;
    memcpy(t->name, start, nl);
    t->name[nl] = 0;
    int cap = 0;
    if (*p == '=') {
        p++;
        while (*p && *p != close) {
            if (*p == '"') { // field name
                p = strchr(p + 1, '"');
                if (!p) break;
                p++;
                continue;
            }
            if (t->nfields == cap) {
                cap = cap ? cap * 2 : 4;
                t->fields = realloc(t->fields, sizeof *t->fields * (size_t)cap);
            }
            memset(&t->fields[t->nfields], 0, sizeof *t->fields);
            p = parse_ty(p, &t->fields[t->nfields], abi);
            t->nfields++;
        }
    }
    if (*p == close) p++;
    t->offsets = calloc(t->nfields ? (size_t)t->nfields : 1, sizeof *t->offsets);
    uint32_t off = 0, align = 1, maxsz = 0;
    for (int i = 0; i < t->nfields; i++) {
        uint32_t a = t->fields[i].align ? t->fields[i].align : 1;
        if (a > align) align = a;
        if (close == '}') {
            off = (off + a - 1) & ~(a - 1);
            t->offsets[i] = off;
            off += t->fields[i].size;
        } else {
            t->offsets[i] = 0;
            if (t->fields[i].size > maxsz) maxsz = t->fields[i].size;
        }
    }
    if (close == ')') off = maxsz;
    t->align = align;
    t->size = (off + align - 1) & ~(align - 1);
    return p;
}

static const char *parse_ty(const char *p, struct ty *t, enum abi abi) {
    p = skip_qualifiers(p);
    t->code = *p;
    switch (*p++) {
    case 'c': case 'C': case 'B': scalar(t, 1, 1, 1, 1, abi); break;
    case 's': case 'S': scalar(t, 2, 2, 2, 2, abi); break;
    case 'i': case 'I': case 'f': scalar(t, 4, 4, 4, 4, abi); break;
    case 'l': case 'L': scalar(t, 4, 4, 8, 8, abi); break;
    case 'q': case 'Q': scalar(t, 8, 4, 8, 8, abi); break;
    case 'd': scalar(t, 8, 4, 8, 8, abi); break;
    case 'D': scalar(t, 16, 16, 16, 16, abi); break;
    case 'v': scalar(t, 0, 1, 0, 1, abi); break;
    case '*': case '#': case ':': case '?': scalar(t, 4, 4, 8, 8, abi); break;
    case '@':
        scalar(t, 4, 4, 8, 8, abi);
        if (*p == '?') {
            t->is_block = true;
            p++;
            if (*p == '<') { // extended block signature
                int depth = 1;
                p++;
                while (*p && depth) {
                    if (*p == '<') depth++;
                    else if (*p == '>') depth--;
                    p++;
                }
            }
        } else if (*p == '"') {
            const char *q = strchr(p + 1, '"');
            p = q ? q + 1 : p + strlen(p);
        }
        break;
    case '^':
        scalar(t, 4, 4, 8, 8, abi);
        t->pointee = calloc(1, sizeof *t->pointee);
        p = parse_ty(p, t->pointee, abi);
        break;
    case '{': p = parse_aggregate(p, t, abi, '}'); break;
    case '(': p = parse_aggregate(p, t, abi, ')'); break;
    case '[': {
        t->count = (uint32_t)strtoul(p, (char **)&p, 10);
        t->fields = calloc(1, sizeof *t->fields);
        t->nfields = 1;
        p = parse_ty(p, &t->fields[0], abi);
        if (*p == ']') p++;
        t->size = t->fields[0].size * t->count;
        t->align = t->fields[0].align;
        break;
    }
    case 'b': // bitfield: treat as int
        strtoul(p, (char **)&p, 10);
        scalar(t, 4, 4, 4, 4, abi);
        t->code = 'I';
        break;
    default:
        LOGE("objc: unknown type code '%c'", p[-1]);
        scalar(t, 4, 4, 8, 8, abi);
        t->code = 'i';
        break;
    }
    return p;
}

// Parsed method signature: return type + argument types (including self/_cmd).
struct sig {
    struct ty ret;
    struct ty args[24];
    int nargs;
};

static void parse_sig_uncached(const char *enc, struct sig *s, enum abi abi) {
    memset(s, 0, sizeof *s);
    const char *p = parse_ty(enc, &s->ret, abi);
    p = skip_digits(p);
    while (*p && s->nargs < 24) {
        p = parse_ty(p, &s->args[s->nargs++], abi);
        p = skip_digits(p);
    }
}

// Parsed trees are never freed, so each distinct encoding is parsed once and
// shared (parsing per message leaked ~400 bytes per call).
struct sig_entry {
    struct sig_entry *next;
    enum abi abi;
    struct sig sig;
    char enc[];
};
static struct sig_entry *sig_cache[1024];
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER;

static void parse_sig(const char *enc, struct sig *s, enum abi abi) {
    uint32_t h = 2166136261u ^ abi;
    for (const char *c = enc; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    struct sig_entry **slot = &sig_cache[h & 1023];
    pthread_mutex_lock(&sig_lock);
    for (struct sig_entry *e = *slot; e; e = e->next) {
        if (e->abi == abi && !strcmp(e->enc, enc)) {
            pthread_mutex_unlock(&sig_lock);
            *s = e->sig;
            return;
        }
    }
    pthread_mutex_unlock(&sig_lock);
    size_t len = strlen(enc);
    struct sig_entry *e = malloc(sizeof *e + len + 1);
    memcpy(e->enc, enc, len + 1);
    e->abi = abi;
    parse_sig_uncached(enc, &e->sig, abi);
    *s = e->sig;
    pthread_mutex_lock(&sig_lock);
    e->next = *slot;
    *slot = e;
    pthread_mutex_unlock(&sig_lock);
}

// Single types through the same cache (parsed as a one-element "signature").
static void parse_ty_cached(const char *enc, struct ty *t, enum abi abi) {
    struct sig s;
    parse_sig(enc, &s, abi);
    *t = s.ret;
}

uint32_t guest_type_size(const char *enc) {
    struct ty t;
    parse_ty_cached(enc, &t, ABI_GUEST);
    return t.size;
}

// i386 Darwin returns structs of size 1, 2, 4, 8 in registers; others in memory.
static bool guest_struct_in_regs(const struct ty *t) {
    return t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8;
}

// --- deriving signatures ---------------------------------------------------------------

static bool geometry_struct(const char *name) {
    static const char *names[] = {"CGPoint", "CGSize", "CGRect", "CGAffineTransform", "CGVector",
                                  "NSEdgeInsets", "_NSPoint", "_NSSize", "_NSRect", "NSDirectionalEdgeInsets"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

// Build a guest encoding from a host one when nothing better is known:
// NSInteger-like 'q' -> 'i', CGFloat inside geometry structs -> 'f'.
static void derive_guest(const char *h, char *out, size_t cap, bool in_geom) {
    size_t o = 0;
    while (*h && o + 2 < cap) {
        char c = *h;
        if (c == '{') {
            const char *eq = h + 1;
            while (*eq && *eq != '=' && *eq != '}') eq++;
            char name[64];
            size_t nl = (size_t)(eq - h - 1);
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, h + 1, nl);
            name[nl] = 0;
            // copy "{name="
            while (h <= eq && *h && o + 2 < cap) out[o++] = *h++;
            int depth = 1;
            const char *body = h;
            while (*h && depth) {
                if (*h == '{') depth++;
                else if (*h == '}') depth--;
                if (depth) h++;
            }
            char inner[512];
            size_t bl = (size_t)(h - body);
            if (bl >= sizeof inner) bl = sizeof inner - 1;
            memcpy(inner, body, bl);
            inner[bl] = 0;
            derive_guest(inner, out + o, cap - o, in_geom || geometry_struct(name));
            o += strlen(out + o);
            if (*h == '}' && o + 1 < cap) out[o++] = *h++;
            continue;
        }
        if (c == 'q') c = 'i';
        else if (c == 'Q') c = 'I';
        else if (c == 'd' && in_geom) c = 'f';
        out[o++] = c;
        h++;
    }
    out[o] = 0;
}

char *host_types_from_guest(const char *gtypes) {
    // Guest-only methods called by the host: keep the encoding (sizes are
    // re-derived per ABI by the parser), widening CGFloat in geometry structs.
    size_t n = strlen(gtypes);
    char *out = malloc(n * 2 + 1);
    size_t o = 0;
    bool in_geom = false;
    for (const char *p = gtypes; *p; p++) {
        if (*p >= '0' && *p <= '9') continue; // drop i386 frame offsets
        if (*p == '{') in_geom = geometry_struct(p + 1) || strncmp(p + 1, "CG", 2) == 0 || strncmp(p + 1, "_NS", 3) == 0;
        if (*p == '}') in_geom = false;
        out[o++] = (*p == 'f' && in_geom) ? 'd' : *p;
    }
    out[o] = 0;
    return out;
}

// --- value conversion ------------------------------------------------------------------

struct fixup {
    int kind; // 1: object out-param, 2: struct in/out, 3: integer in/out
    gaddr_t guest;
    void *host;
    struct ty *gt, *ht;
};
struct fixups {
    struct fixup f[24];
    int n;
    void *temps[24];
    int ntemps;
};

static int64_t read_gint(const struct ty *t, gaddr_t a) {
    switch (t->code) {
    case 'c': return (int8_t)gread8(a);
    case 'C': case 'B': return gread8(a);
    case 's': return (int16_t)gread16(a);
    case 'S': return gread16(a);
    case 'i': case 'l': return (int32_t)gread32(a);
    case 'I': case 'L': return gread32(a);
    case 'q': case 'Q': return (int64_t)(gread32(a) | ((uint64_t)gread32(a + 4) << 32));
    case 'f': { float f; uint32_t v = gread32(a); memcpy(&f, &v, 4); return (int64_t)f; }
    case 'd': { double d; uint64_t v = gread32(a) | ((uint64_t)gread32(a + 4) << 32); memcpy(&d, &v, 8); return (int64_t)d; }
    default: return (int32_t)gread32(a);
    }
}

static double read_gfloat(const struct ty *t, gaddr_t a) {
    if (t->code == 'f') {
        float f;
        uint32_t v = gread32(a);
        memcpy(&f, &v, 4);
        return f;
    }
    if (t->code == 'd') {
        double d;
        uint64_t v = gread32(a) | ((uint64_t)gread32(a + 4) << 32);
        memcpy(&d, &v, 8);
        return d;
    }
    return (double)read_gint(t, a);
}

static void write_hint(uint8_t *d, uint32_t size, int64_t v) {
    switch (size) {
    case 1: *(int8_t *)d = (int8_t)v; break;
    case 2: *(int16_t *)d = (int16_t)v; break;
    case 4: *(int32_t *)d = (int32_t)v; break;
    default: *(int64_t *)d = v; break;
    }
}

static bool same_layout(const struct ty *g, const struct ty *h) {
    if (g->size != h->size) return false;
    if (g->code == '{' || g->code == '(' || g->code == '[') {
        if (g->nfields != h->nfields) return false;
        for (int i = 0; i < g->nfields; i++)
            if (g->offsets[i] != h->offsets[i] || !same_layout(&g->fields[i], &h->fields[i])) return false;
        return true;
    }
    if (strchr("@#:*^?", g->code) || strchr("@#:*^?", h->code)) return false;
    return g->code == h->code || (strchr("iI", g->code) && strchr("iI", h->code)) ||
           (strchr("cCB", g->code) && strchr("cCB", h->code)) || (strchr("sS", g->code) && strchr("sS", h->code)) ||
           (strchr("qQ", g->code) && strchr("qQ", h->code));
}

static void conv_g2h(const struct ty *gt, gaddr_t src, const struct ty *ht, uint8_t *dst, struct fixups *fx);
static void conv_h2g(const struct ty *ht, const uint8_t *src, const struct ty *gt, gaddr_t dst);

static void conv_g2h(const struct ty *gt, gaddr_t src, const struct ty *ht, uint8_t *dst, struct fixups *fx) {
    switch (ht->code) {
    case 'c': case 'C': case 'B': case 's': case 'S': case 'i': case 'I': case 'l': case 'L': case 'q': case 'Q':
        write_hint(dst, ht->size, read_gint(gt, src));
        break;
    case 'f': { float f = (float)read_gfloat(gt, src); memcpy(dst, &f, 4); break; }
    case 'd': { double d = read_gfloat(gt, src); memcpy(dst, &d, 8); break; }
    case '@': case '#': { id o = g2o(gread32(src)); memcpy(dst, &o, sizeof o); break; }
    case ':': { SEL s = gsel_to_host(gread32(src)); memcpy(dst, &s, sizeof s); break; }
    case '*': { void *p = g2h(gread32(src)); memcpy(dst, &p, sizeof p); break; }
    case '?': {
        gaddr_t g = gread32(src);
        if (g) LOGE("objc: guest function pointer %#x passed to host (not supported)", g);
        void *p = NULL;
        memcpy(dst, &p, sizeof p);
        break;
    }
    case '^': {
        gaddr_t g = gread32(src);
        void *p = NULL;
        const struct ty *hp = ht->pointee, *gp = gt->code == '^' ? gt->pointee : NULL;
        if (g && hp && gp && hp->code != 'v' && !same_layout(gp, hp) && hp->size && fx && fx->n < 24) {
            // Layout differs (e.g. NSError **, NSRect *, NSInteger *): temp host copy, written back after the call.
            p = calloc(1, hp->size);
            conv_g2h(gp, g, hp, p, NULL);
            fx->f[fx->n++] = (struct fixup){hp->code == '@' ? 1 : 2, g, p, (struct ty *)gp, (struct ty *)hp};
        } else if (g && hobj_is_handle(g)) {
            p = hobj_unwrap(g); // opaque host object passed back by the guest
        } else if (g) {
            p = g2h(g);
        }
        memcpy(dst, &p, sizeof p);
        break;
    }
    case '{': case '(': case '[': {
        if (gt->nfields == ht->nfields && gt->code == ht->code) {
            for (int i = 0; i < ht->nfields; i++) {
                uint32_t n = ht->code == '[' ? ht->count : 1;
                for (uint32_t k = 0; k < n; k++)
                    conv_g2h(&gt->fields[i], src + gt->offsets[i] + k * gt->fields[i].size, &ht->fields[i],
                             dst + ht->offsets[i] + k * ht->fields[i].size, fx);
            }
        } else {
            memcpy(dst, g2h(src), gt->size < ht->size ? gt->size : ht->size);
        }
        break;
    }
    case 'v': break;
    default: memcpy(dst, g2h(src), ht->size < 4 ? ht->size : 4); break;
    }
}

static void write_gint(const struct ty *t, gaddr_t a, int64_t v) {
    switch (t->size) {
    case 1: gwrite8(a, (uint8_t)v); break;
    case 2: gwrite16(a, (uint16_t)v); break;
    case 8: gwrite32(a, (uint32_t)v); gwrite32(a + 4, (uint32_t)((uint64_t)v >> 32)); break;
    default: gwrite32(a, (uint32_t)v); break;
    }
}

static int64_t read_hint(const struct ty *t, const uint8_t *p) {
    switch (t->code) {
    case 'c': return *(const int8_t *)p;
    case 'C': case 'B': return *p;
    case 's': return *(const int16_t *)p;
    case 'S': return *(const uint16_t *)p;
    case 'i': return *(const int32_t *)p;
    case 'I': return *(const uint32_t *)p;
    case 'f': return (int64_t) * (const float *)p;
    case 'd': return (int64_t) * (const double *)p;
    default: return *(const int64_t *)p;
    }
}

static void conv_h2g(const struct ty *ht, const uint8_t *src, const struct ty *gt, gaddr_t dst) {
    switch (gt->code) {
    case 'c': case 'C': case 'B': case 's': case 'S': case 'i': case 'I': case 'l': case 'L': case 'q': case 'Q':
        write_gint(gt, dst, read_hint(ht, src));
        break;
    case 'f': {
        float f = ht->code == 'd' ? (float)*(const double *)src : ht->code == 'f' ? *(const float *)src : (float)read_hint(ht, src);
        uint32_t v;
        memcpy(&v, &f, 4);
        gwrite32(dst, v);
        break;
    }
    case 'd': {
        double d = ht->code == 'f' ? *(const float *)src : ht->code == 'd' ? *(const double *)src : (double)read_hint(ht, src);
        uint64_t v;
        memcpy(&v, &d, 8);
        gwrite32(dst, (uint32_t)v);
        gwrite32(dst + 4, (uint32_t)(v >> 32));
        break;
    }
    case '@': case '#': { id o; memcpy(&o, src, sizeof o); gwrite32(dst, o2g(o)); break; }
    case ':': { SEL s; memcpy(&s, src, sizeof s); gwrite32(dst, host_sel_to_guest(s)); break; }
    case '*': case '^': case '?': {
        void *p;
        memcpy(&p, src, sizeof p);
        if (p && !host_ptr_in_guest(p)) {
            if (gt->code == '*') {
                gwrite32(dst, guest_static_string(5, p)); // host C string -> guest copy
            } else {
                // Opaque host pointers (CGLContextObj, CGContextRef, ...) become
                // handles, the same ones the C API thunks use.
                gwrite32(dst, hobj_wrap(p));
            }
        } else
            gwrite32(dst, p ? h2g(p) : 0);
        break;
    }
    case '{': case '(': case '[':
        if (gt->nfields == ht->nfields && gt->code == ht->code) {
            for (int i = 0; i < gt->nfields; i++) {
                uint32_t n = gt->code == '[' ? gt->count : 1;
                for (uint32_t k = 0; k < n; k++)
                    conv_h2g(&ht->fields[i], src + ht->offsets[i] + k * ht->fields[i].size, &gt->fields[i],
                             dst + gt->offsets[i] + k * gt->fields[i].size);
            }
        } else {
            memcpy(g2h(dst), src, gt->size < ht->size ? gt->size : ht->size);
        }
        break;
    default: break;
    }
}

static void run_fixups(struct fixups *fx) {
    for (int i = 0; i < fx->n; i++) {
        struct fixup *f = &fx->f[i];
        conv_h2g(f->ht, f->host, f->gt, f->guest);
        free(f->host);
    }
}

// --- libffi --------------------------------------------------------------------------------

static ffi_type *ffi_for(const struct ty *t) {
    switch (t->code) {
    case 'c': return &ffi_type_sint8;
    case 'C': case 'B': return &ffi_type_uint8;
    case 's': return &ffi_type_sint16;
    case 'S': return &ffi_type_uint16;
    case 'i': return &ffi_type_sint32;
    case 'I': return &ffi_type_uint32;
    case 'l': case 'q': return &ffi_type_sint64;
    case 'L': case 'Q': return &ffi_type_uint64;
    case 'f': return &ffi_type_float;
    case 'd': return &ffi_type_double;
    case 'D': return &ffi_type_longdouble;
    case 'v': return &ffi_type_void;
    case '{': case '(': case '[': {
        ffi_type *st = calloc(1, sizeof *st);
        st->type = FFI_TYPE_STRUCT;
        int n = t->code == '[' ? (int)t->count : t->nfields;
        if (t->code == '(') n = 1; // unions: approximate by the largest member
        st->elements = calloc((size_t)n + 1, sizeof(ffi_type *));
        if (t->code == '(') {
            int big = 0;
            for (int i = 1; i < t->nfields; i++)
                if (t->fields[i].size > t->fields[big].size) big = i;
            st->elements[0] = ffi_for(&t->fields[big]);
        } else {
            for (int i = 0; i < n; i++) st->elements[i] = ffi_for(&t->fields[t->code == '[' ? 0 : i]);
        }
        return st;
    }
    default: return &ffi_type_pointer;
    }
}

static void free_ffi(ffi_type *t) {
    if (t->type != FFI_TYPE_STRUCT) return;
    for (ffi_type **e = t->elements; *e; e++) free_ffi(*e);
    free(t->elements);
    free(t);
}

static uint32_t slot_size(const struct ty *t) { return (t->size + 3) & ~3u; }

void marshal_call_host(id recv, SEL sel, IMP imp, struct mcall *mc) {
    struct sig hs, gs;
    parse_sig(mc->htypes, &hs, ABI_HOST);
    char derived[1024];
    const char *gtypes = mc->gtypes;
    if (!gtypes) {
        derive_guest(mc->htypes, derived, sizeof derived, false);
        gtypes = derived;
    }
    parse_sig(gtypes, &gs, ABI_GUEST);
    if (gs.nargs != hs.nargs) {
        LOGE("objc: signature mismatch for %s: guest %s host %s", sel_getName(sel), gtypes, mc->htypes);
        derive_guest(mc->htypes, derived, sizeof derived, false);
        parse_sig(derived, &gs, ABI_GUEST);
    }
    int n = hs.nargs;
    ffi_type *atypes[24];
    void *avalues[24];
    uint8_t storage[24][64];
    struct fixups fx = {0};
    memcpy(storage[0], &recv, sizeof recv);
    memcpy(storage[1], &sel, sizeof sel);
    gaddr_t ga = mc->args_at;
    for (int i = 0; i < n; i++) {
        atypes[i] = ffi_for(&hs.args[i]);
        void *slot = hs.args[i].size <= 64 ? storage[i] : calloc(1, hs.args[i].size);
        if (hs.args[i].size > 64) fx.temps[fx.ntemps++] = slot;
        if (i >= 2) {
            conv_g2h(&gs.args[i], ga, &hs.args[i], slot, &fx);
            ga += slot_size(&gs.args[i]);
        }
        avalues[i] = slot;
    }
    ffi_type *rtype = ffi_for(&hs.ret);
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, atypes) != FFI_OK)
        fatal("objc: ffi_prep_cif failed for %s (%s)", sel_getName(sel), mc->htypes);
    uint8_t rbuf[256] __attribute__((aligned(16)));
    memset(rbuf, 0, sizeof rbuf);
    void *rv = hs.ret.size > sizeof rbuf ? calloc(1, hs.ret.size) : rbuf;
    ffi_call(&cif, FFI_FN(imp), rv, avalues);
    run_fixups(&fx);
    for (int i = 0; i < fx.ntemps; i++) free(fx.temps[i]);

    // Return value to guest
    mc->ret = 0;
    mc->fp_ret = false;
    const struct ty *gr = &gs.ret, *hr = &hs.ret;
    if (gr->code == 'f' || gr->code == 'd') {
        mc->fp_ret = true;
        mc->fret = hr->code == 'f' ? *(float *)rv : hr->code == 'd' ? *(double *)rv : (double)read_hint(hr, rv);
    } else if (gr->code == '{' || gr->code == '(' || gr->code == '[') {
        if (mc->stret_ptr) {
            conv_h2g(hr, rv, gr, mc->stret_ptr);
            mc->ret = mc->stret_ptr;
        } else {
            gaddr_t tmp = gheap_calloc(1, 16);
            conv_h2g(hr, rv, gr, tmp);
            mc->ret = gread32(tmp) | ((uint64_t)gread32(tmp + 4) << 32);
            gheap_free(tmp);
        }
    } else if (gr->code != 'v') {
        gaddr_t tmp = gheap_calloc(1, 16);
        conv_h2g(hr, rv, gr, tmp);
        mc->ret = gr->size == 8 ? (gread32(tmp) | ((uint64_t)gread32(tmp + 4) << 32)) : gread32(tmp);
        if (gr->size < 4 && strchr("cs", gr->code)) mc->ret = (uint32_t)(int32_t)read_gint(gr, tmp);
        gheap_free(tmp);
    }
    if (rv != rbuf) free(rv);
    for (int i = 0; i < n; i++) free_ffi(atypes[i]);
    free_ffi(rtype);
}

// --- host invocation -> guest IMP ------------------------------------------------------------

void marshal_invocation_to_guest(NSInvocation *inv, gaddr_t gself, gaddr_t gimp, gaddr_t gsel, const char *gtypes) {
    NSMethodSignature *ms = inv.methodSignature;
    struct sig gs;
    parse_sig(gtypes, &gs, ABI_GUEST);
    NSUInteger n = ms.numberOfArguments;
    uint32_t words[64];
    int nw = 0;
    gaddr_t gret_buf = 0;
    bool stret = (gs.ret.code == '{' || gs.ret.code == '(' || gs.ret.code == '[') && !guest_struct_in_regs(&gs.ret);
    if (stret) {
        gret_buf = gheap_calloc(1, gs.ret.size + 16);
        words[nw++] = gret_buf;
    }
    words[nw++] = gself;
    words[nw++] = gsel;
    gaddr_t scratch = gheap_calloc(1, 256);
    for (NSUInteger i = 2; i < n && (int)i < gs.nargs; i++) {
        struct ty ht = {0};
        parse_ty_cached([ms getArgumentTypeAtIndex:i], &ht, ABI_HOST);
        uint8_t hbuf[128] = {0};
        [inv getArgument:hbuf atIndex:i];
        memset(g2h(scratch), 0, 256);
        conv_h2g(&ht, hbuf, &gs.args[i], scratch);
        uint32_t sz = slot_size(&gs.args[i]);
        for (uint32_t k = 0; k < sz / 4 && nw < 64; k++) words[nw++] = gread32(scratch + 4 * k);
    }
    gheap_free(scratch);
    struct cpu_call_result r;
    bool fp = gs.ret.code == 'f' || gs.ret.code == 'd';
    cpu_call_ex(gimp, nw, words, fp, &r);

    struct ty hr = {0};
    parse_ty_cached(ms.methodReturnType, &hr, ABI_HOST);
    if (hr.code == 'v' || ms.methodReturnLength == 0) {
        if (gret_buf) gheap_free(gret_buf);
        return;
    }
    uint8_t hret[256] = {0};
    gaddr_t gtmp = gret_buf ? gret_buf : gheap_calloc(1, 16);
    if (!gret_buf) {
        if (fp) {
            if (gs.ret.code == 'f') {
                float f = (float)r.fret;
                memcpy(g2h(gtmp), &f, 4);
            } else
                memcpy(g2h(gtmp), &r.fret, 8);
        } else {
            gwrite32(gtmp, (uint32_t)r.ret);
            gwrite32(gtmp + 4, (uint32_t)(r.ret >> 32));
        }
    }
    conv_g2h(&gs.ret, gtmp, &hr, hret, NULL);
    [inv setReturnValue:hret];
    gheap_free(gtmp);
}
