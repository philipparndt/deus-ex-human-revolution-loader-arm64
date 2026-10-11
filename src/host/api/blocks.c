// Blocks runtime for guest blocks (libclosure semantics on guest memory).
//
// i386 layouts:
//   struct Block_layout { isa; int flags; int reserved; invoke; descriptor; captures... }
//   struct Block_descriptor { reserved; size; [copy; dispose] if HAS_COPY_DISPOSE; [signature] }
//   struct Block_byref { isa; forwarding; int flags; int size; [keep; destroy] if HAS_COPY_DISPOSE; ... }
#include "blocks.h"

#include "common.h"

#include <pthread.h>

enum {
    BLOCK_DEALLOCATING = 0x0001,
    BLOCK_REFCOUNT_MASK = 0xfffe,
    BLOCK_NEEDS_FREE = 1 << 24,
    BLOCK_HAS_COPY_DISPOSE = 1 << 25,
    BLOCK_IS_GC = 1 << 27,
    BLOCK_IS_GLOBAL = 1 << 28,
    BLOCK_HAS_SIGNATURE = 1 << 30,
    BLOCK_BYREF_HAS_COPY_DISPOSE = 1 << 25,
};
enum {
    BLOCK_FIELD_IS_OBJECT = 3,
    BLOCK_FIELD_IS_BLOCK = 7,
    BLOCK_FIELD_IS_BYREF = 8,
    BLOCK_FIELD_IS_WEAK = 16,
    BLOCK_BYREF_CALLER = 128,
};

static void init_isa_marker(gaddr_t a) { gwrite32(a, a); }
GDATA(_NSConcreteGlobalBlock, 32, init_isa_marker);
GDATA(_NSConcreteStackBlock, 32, init_isa_marker);
GDATA(_NSConcreteMallocBlock, 32, init_isa_marker);
GDATA(_NSConcreteAutoBlock, 32, init_isa_marker);
GDATA(_NSConcreteFinalizingBlock, 32, init_isa_marker);
GDATA(_NSConcreteWeakBlockVariable, 32, init_isa_marker);

// Hooks for retaining/releasing guest Objective-C objects captured by blocks;
// the ObjC bridge installs these.
gaddr_t (*block_objc_retain)(gaddr_t obj);
void (*block_objc_release)(gaddr_t obj);

static uint32_t *flags_ptr(gaddr_t b) { return (uint32_t *)g2h(b + 4); }

static bool latching_incr(uint32_t *where) {
    for (;;) {
        uint32_t old = __atomic_load_n(where, __ATOMIC_RELAXED);
        if ((old & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) return true;
        if (__atomic_compare_exchange_n(where, &old, old + 2, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return true;
    }
}

// Returns true when the count dropped to zero.
static bool latching_decr(uint32_t *where) {
    for (;;) {
        uint32_t old = __atomic_load_n(where, __ATOMIC_RELAXED);
        if ((old & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) return false;
        if ((old & BLOCK_REFCOUNT_MASK) == 0) return false;
        uint32_t nv = old - 2;
        bool zero = (old & BLOCK_REFCOUNT_MASK) == 2;
        if (zero) nv = old - 1; // set deallocating
        if (__atomic_compare_exchange_n(where, &old, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return zero;
    }
}

gaddr_t gblock_copy(gaddr_t b) {
    if (!b) return 0;
    uint32_t flags = gread32(b + 4);
    if (flags & BLOCK_NEEDS_FREE) {
        latching_incr(flags_ptr(b));
        return b;
    }
    if (flags & BLOCK_IS_GLOBAL) return b;
    gaddr_t desc = gread32(b + 16);
    uint32_t size = gread32(desc + 4);
    gaddr_t nb = gheap_alloc(size);
    memcpy(g2h(nb), g2h(b), size);
    uint32_t nf = (flags & ~(BLOCK_REFCOUNT_MASK | BLOCK_DEALLOCATING)) | BLOCK_NEEDS_FREE | 2;
    gwrite32(nb + 4, nf);
    gwrite32(nb, hostsym_lookup("__NSConcreteMallocBlock"));
    if (flags & BLOCK_HAS_COPY_DISPOSE) gcall2(gread32(desc + 8), nb, b);
    return nb;
}

void gblock_release(gaddr_t b) {
    if (!b) return;
    uint32_t flags = gread32(b + 4);
    if (flags & BLOCK_IS_GLOBAL) return;
    if (!(flags & BLOCK_NEEDS_FREE)) return;
    if (latching_decr(flags_ptr(b))) {
        gaddr_t desc = gread32(b + 16);
        if (flags & BLOCK_HAS_COPY_DISPOSE) gcall1(gread32(desc + 12), b);
        gheap_free(b);
    }
}

THUNK(_Block_copy) { RET(gblock_copy(ARG(0))); }
THUNK(_Block_release) { gblock_release(ARG(0)); }

static gaddr_t byref_copy(gaddr_t arg) {
    gaddr_t src = gread32(arg + 4); // forwarding
    uint32_t sflags = gread32(src + 8);
    if ((sflags & BLOCK_REFCOUNT_MASK) == 0) {
        uint32_t size = gread32(src + 12);
        gaddr_t copy = gheap_alloc(size);
        gwrite32(copy, 0);
        gwrite32(copy + 8, sflags | BLOCK_NEEDS_FREE | 4); // one for caller, one for stack
        gwrite32(copy + 4, copy);
        gwrite32(src + 4, copy);
        gwrite32(copy + 12, size);
        if (sflags & BLOCK_BYREF_HAS_COPY_DISPOSE) {
            gwrite32(copy + 16, gread32(src + 16));
            gwrite32(copy + 20, gread32(src + 20));
            gcall2(gread32(src + 16), copy, src);
        } else if (size > 16) {
            memcpy(g2h(copy + 16), g2h(src + 16), size - 16);
        }
        return copy;
    }
    if ((sflags & BLOCK_NEEDS_FREE)) latching_incr((uint32_t *)g2h(src + 8));
    return src;
}

static void byref_release(gaddr_t arg) {
    gaddr_t b = gread32(arg + 4);
    uint32_t flags = gread32(b + 8);
    if (!(flags & BLOCK_NEEDS_FREE)) return;
    if (latching_decr((uint32_t *)g2h(b + 8))) {
        if (flags & BLOCK_BYREF_HAS_COPY_DISPOSE) gcall1(gread32(b + 20), b);
        gheap_free(b);
    }
}

THUNK(_Block_object_assign) {
    gaddr_t dst = ARG(0), obj = ARG(1);
    uint32_t flags = ARG(2);
    switch (flags & (BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_BLOCK | BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK |
                     BLOCK_BYREF_CALLER)) {
    case BLOCK_FIELD_IS_OBJECT:
        if (block_objc_retain) block_objc_retain(obj);
        gwrite32(dst, obj);
        break;
    case BLOCK_FIELD_IS_BLOCK: gwrite32(dst, gblock_copy(obj)); break;
    case BLOCK_FIELD_IS_BYREF:
    case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK: gwrite32(dst, byref_copy(obj)); break;
    default: gwrite32(dst, obj); break; // BYREF_CALLER variants: plain assignment
    }
}

THUNK(_Block_object_dispose) {
    gaddr_t obj = ARG(0);
    uint32_t flags = ARG(1);
    switch (flags & (BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_BLOCK | BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK |
                     BLOCK_BYREF_CALLER)) {
    case BLOCK_FIELD_IS_BYREF:
    case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK: byref_release(obj); break;
    case BLOCK_FIELD_IS_BLOCK: gblock_release(obj); break;
    case BLOCK_FIELD_IS_OBJECT:
        if (block_objc_release) block_objc_release(obj);
        break;
    default: break;
    }
}

void gblock_invoke0(gaddr_t b) { gcall1(gread32(b + 12), b); }
