// CoreFoundation bridge: guest <-> host CF/NS object handles.
//
// CF objects are toll-free bridged with ObjC objects, so both share the hobj
// handle space: a guest-visible CF/NS object is a hobj handle (hobj.h) whose
// slot points at the host object.
//
//   CFTypeRef h = gcf(ARG(0));      // guest handle -> host object (NULL for 0)
//   RET(cfg(CFStringCreateCopy(...)));   // host object -> guest handle
//
// Special guest objects recognised by gcf():
//   - constant CFString literals in the guest's __DATA,__cfstring sections
//     ({isa = &___CFConstantStringClassReference, flags, chars, length}, 16 bytes
//     on i386). They map to immortal host strings, and cfg() maps those host
//     strings back to the original guest literal address.
//   - guest-side objects registered by another bridge (ObjC) through
//     cf_bridge_set_foreign_resolver().
//
// Ownership: retain counts live in the host object. Guest CFRetain/CFRelease
// forward to the host object; handles stay stable for the object's lifetime.
#pragma once
#include "common.h"

#include <CoreFoundation/CoreFoundation.h>

CFTypeRef gcf(gaddr_t g);
gaddr_t cfg(CFTypeRef h);
static inline CFStringRef gcf_string(gaddr_t g) { return (CFStringRef)gcf(g); }

// Like cfg() but for a host object the guest receives with +1 ownership that
// should not be released on the host side (e.g. a "Create"/"Copy" result).
// Identical to cfg(); kept as a separate name to document intent.
#define cfg_owned(h) cfg((CFTypeRef)(h))

// True if g is the address of a guest constant CFString literal.
bool cf_is_guest_constant(gaddr_t g);
// Guest address of ___CFConstantStringClassReference.
gaddr_t cf_constant_string_class(void);

// Hooks for the ObjC bridge.
//  - resolver: called by gcf() for non-handle, non-literal guest addresses
//    (e.g. guest-implemented ObjC objects); return NULL if unknown.
//  - wrap hook: called by cfg() whenever a handle's isa word is still 0, so the
//    ObjC bridge can install a guest-visible isa.
typedef CFTypeRef (*cf_foreign_resolver)(gaddr_t g);
typedef void (*cf_wrap_hook)(gaddr_t handle, CFTypeRef obj);
void cf_bridge_set_foreign_resolver(cf_foreign_resolver fn);
void cf_bridge_set_wrap_hook(cf_wrap_hook fn);

// --- thunk helpers -------------------------------------------------------------
#define ARGCF(i) gcf(ARG(i))
#define ARGSTR(i) gcf_string(ARG(i))
#define ARGIDX(i) ((CFIndex)ARGI(i)) // CFIndex is 32-bit in the guest
#define ARGBOOL(i) ((Boolean)(ARG(i) & 0xff))
#define RET_CF(x) RET(cfg((CFTypeRef)(x)))
#define RET_BOOL(x) RET((x) ? 1 : 0)
// CFRange: two stack slots in, edx:eax out.
#define ARGRANGE(i) CFRangeMake(ARGIDX(i), ARGIDX((i) + 1))
#define RET_RANGE(rg)                                                                              \
    do {                                                                                           \
        CFRange rr_ = (rg);                                                                        \
        RET64((uint64_t)(uint32_t)rr_.location | ((uint64_t)(uint32_t)rr_.length << 32));           \
    } while (0)
static inline void put_grange(gaddr_t g, CFRange rg) {
    if (!g) return;
    gwrite32(g, (uint32_t)rg.location);
    gwrite32(g + 4, (uint32_t)rg.length);
}

// Guest allocator handle -> host allocator. 0 means default. Guest allocators
// created with CFAllocatorCreate are host allocators calling back into the guest.
CFAllocatorRef gcf_allocator(gaddr_t g);
// A host allocator that allocates from the guest heap (gheap_alloc/gheap_free).
// Objects (and their storage) created with it are directly guest-accessible.
CFAllocatorRef cf_guest_heap_allocator(void);
// Host allocator to use as the contents deallocator for guest-owned buffers
// passed to *NoCopy APIs (0/default -> guest heap free, kCFAllocatorNull -> none).
CFAllocatorRef cf_guest_deallocator(gaddr_t g);

// --- strings -------------------------------------------------------------------
// Host UTF-8 copy (malloc'ed, caller frees); NULL for NULL.
char *cf_utf8(CFStringRef s);
// Guest heap copy of a CF string as UTF-8; 0 for NULL.
gaddr_t cf_guest_utf8(CFStringRef s);
// New host CFString from a guest C string (UTF-8, falling back to MacRoman).
CFStringRef cf_string_from_guest(gaddr_t s);

// --- lifetime-bound attachments --------------------------------------------------
// Attach host data to a host object; dtor(ptr) runs when the object is
// deallocated (or never for immortal/tagged objects). Replaces a previous
// attachment with the same key.
void cf_attach(CFTypeRef obj, const void *key, void *ptr, void (*dtor)(void *));
void *cf_attached(CFTypeRef obj, const void *key);
// A guest buffer with a copy of size bytes of host data, kept alive as long as
// obj and refreshed on every call (for "returns interior pointer" APIs).
gaddr_t cf_guest_copy(CFTypeRef obj, const void *key, const void *data, uint32_t size);

// --- containers -------------------------------------------------------------------
// Containers created by the guest with NULL callbacks hold raw 32-bit words.
enum { CF_RAW_KEYS = 1, CF_RAW_VALUES = 2 };
void cf_set_raw_flags(CFTypeRef container, unsigned flags);
unsigned cf_raw_flags(CFTypeRef container);
// Element conversion according to raw flags.
static inline const void *cf_elem_from_guest(bool raw, uint32_t w) {
    return raw ? (const void *)(uintptr_t)w : gcf(w);
}
static inline uint32_t cf_elem_to_guest(bool raw, const void *v) {
    return raw ? (uint32_t)(uintptr_t)v : cfg(v);
}

// --- guest context structs ({CFIndex version; info; retain; release; copyDescription}) ---
struct gcf_context {
    gaddr_t version, info, retain, release, copydesc;
};
// Read a guest context (NULL ctx -> zeros). If retain is set it is called on info.
void gcf_context_read(gaddr_t ctx, struct gcf_context *out, bool call_retain);
// Call the guest release callback on info, if any.
void gcf_context_release(const struct gcf_context *c);
void gcf_context_write(gaddr_t ctx, const struct gcf_context *c);

// Format a CFString-style format string (supports %@) with guest varargs.
CFStringRef cf_format_guest(CFStringRef fmt, struct gva va);

// The game's main bundle (the .app containing the guest executable), or the
// host main bundle when there is none.
CFBundleRef cf_main_bundle(void);
