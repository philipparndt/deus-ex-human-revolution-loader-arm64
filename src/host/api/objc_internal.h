// Objective-C bridge between the guest's legacy (fragile, i386 "objc1") runtime
// data and the host's modern Objective-C runtime.
//
// Object model:
//  - Host objects (NSWindow, NSString, ...) appear in the guest as hobj handles.
//  - Guest-defined classes (FRL*, PL*, ...) stay in guest memory (their legacy
//    objc_class structs). Each gets a host "shadow" subclass of its nearest host
//    ancestor (DXG_<name>), created on demand. Every guest instance is a guest
//    memory block (guest ivar layout, isa = guest class) paired with an instance
//    of the shadow class; the shadow forwards guest-implemented selectors back
//    into the guest (forwardInvocation:).
//  - Selectors: the guest sees unique guest C strings; host SELs are looked up
//    by name.
#pragma once
#include "common.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

// i386 legacy runtime structures (offsets).
enum {
    GCLS_ISA = 0,
    GCLS_SUPER = 4,
    GCLS_NAME = 8,
    GCLS_VERSION = 12,
    GCLS_INFO = 16,
    GCLS_INSTANCE_SIZE = 20,
    GCLS_IVARS = 24,
    GCLS_METHODS = 28,
    GCLS_CACHE = 32,
    GCLS_PROTOCOLS = 36,
};
#define CLS_CLASS 0x1
#define CLS_META 0x2
#define CLS_NO_METHOD_ARRAY 0x4000

struct gmethod {
    gaddr_t sel; // unique guest SEL
    gaddr_t types;
    gaddr_t imp;
};

struct gclass {
    gaddr_t cls, meta;
    const char *name;
    struct gclass *super_g; // guest superclass (NULL if the superclass is a host class)
    Class host_ancestor;    // nearest host class in the superclass chain
    Class shadow;           // DXG_<name> host subclass (lazy)
    uint32_t instance_size;
    struct gmethod *imethods, *cmethods;
    int nimethods, ncmethods, cap_i, cap_c;
};

// --- runtime (objc_rt.m) ---
void objc_bridge_register_images(void);
gaddr_t gsel_unique(const char *name);   // guest SEL for a name (created on demand)
const char *gsel_name(gaddr_t sel);      // name of a guest SEL (or C string)
SEL gsel_to_host(gaddr_t sel);
gaddr_t host_sel_to_guest(SEL sel);
struct gclass *gclass_by_addr(gaddr_t a); // guest class or metaclass struct -> record
struct gclass *gclass_by_name(const char *name);
bool gaddr_is_gclass_meta(gaddr_t a);
// Method lookup through the guest superclass chain (stops at the first host class).
const struct gmethod *gclass_find_method(struct gclass *c, gaddr_t sel, bool class_method);
gaddr_t host_class_handle(Class c); // class or metaclass -> guest handle
Class guest_class_ref_to_host(gaddr_t ref); // handle / guest class -> host Class (shadow for guest)

// --- objects (objc_conv.m) ---
id g2o(gaddr_t g);           // guest object -> host object (nil for 0)
gaddr_t o2g(id o);           // host object -> guest object/handle
gaddr_t gobj_alloc_instance(struct gclass *c); // +alloc for a guest class
Class gclass_shadow(struct gclass *c);
// Guest instance block for a shadow object (0 if not a shadow object).
gaddr_t shadow_guest(id o);
id guest_instance_host(gaddr_t g); // paired host object of a guest instance (nil if none)
void guest_instance_forget(gaddr_t g);

// --- marshalling (objc_marshal.m) ---
struct mcall {
    // in
    gaddr_t args_at;      // guest address of the first argument after _cmd
    const char *gtypes;   // guest i386 encoding (may be NULL -> derived from htypes)
    const char *htypes;   // host encoding of the method
    gaddr_t stret_ptr;    // guest hidden struct return pointer (0 if none)
    // out
    uint64_t ret;         // edx:eax
    double fret;
    bool fp_ret;
};
// Call host IMP for (receiver, sel) converting guest arguments.
void marshal_call_host(id recv, SEL sel, IMP imp, struct mcall *mc);
// Forward a host NSInvocation into a guest IMP (shadow classes).
void marshal_invocation_to_guest(NSInvocation *inv, gaddr_t gself, gaddr_t gimp, gaddr_t gsel, const char *gtypes);
// Size of a guest return value type (for stret decisions).
uint32_t guest_type_size(const char *enc);
// Derive a host encoding from a guest one (for guest-only methods called by the host).
char *host_types_from_guest(const char *gtypes);

// --- signatures (objc_sigs_gen.c, generated) ---
const char *objc_guest_types_for(Class cls, SEL sel, bool class_method);
