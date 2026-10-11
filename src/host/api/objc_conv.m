// Object conversion between guest and host, and the host shadow classes that
// represent guest-defined classes.
#import "objc_internal.h"

#import <Foundation/Foundation.h>
#import <objc/message.h>
#include "blocks.h"
#include "cf_bridge.h"
#include "../strmap.h"

#include <pthread.h>

static pthread_mutex_t conv_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

// guest instance address -> host object (unretained; the host object owns the pairing)
static struct { gaddr_t key; void *val; } *inst_map;
static uint32_t inst_cap, inst_count;

static void inst_put(gaddr_t k, void *v) {
    if ((inst_count + 1) * 2 > inst_cap) {
        uint32_t oc = inst_cap;
        __typeof__(inst_map) old = inst_map;
        inst_cap = inst_cap ? inst_cap * 2 : 1024;
        inst_map = calloc(inst_cap, sizeof *inst_map);
        inst_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i].key && old[i].val) inst_put(old[i].key, old[i].val);
        free(old);
    }
    uint32_t j = (k * 2654435761u) & (inst_cap - 1);
    while (inst_map[j].key && inst_map[j].key != k) j = (j + 1) & (inst_cap - 1);
    if (!inst_map[j].key) inst_count++;
    inst_map[j].key = k;
    inst_map[j].val = v;
}

static void *inst_get(gaddr_t k) {
    if (!inst_cap) return NULL;
    uint32_t j = (k * 2654435761u) & (inst_cap - 1);
    while (inst_map[j].key) {
        if (inst_map[j].key == k) return inst_map[j].val;
        j = (j + 1) & (inst_cap - 1);
    }
    return NULL;
}

id guest_instance_host(gaddr_t g) {
    pthread_mutex_lock(&conv_lock);
    void *h = inst_get(g);
    pthread_mutex_unlock(&conv_lock);
    return (__bridge id)h;
}

void guest_instance_forget(gaddr_t g) {
    pthread_mutex_lock(&conv_lock);
    inst_put(g, NULL);
    pthread_mutex_unlock(&conv_lock);
}

// --- shadow classes -----------------------------------------------------------------------

#define SHADOW_IVAR "_dx_guest"
static struct strmap shadow_owner; // shadow class name -> struct gclass*

static struct gclass *gclass_of_shadow(Class cls) {
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        uint64_t v;
        if (strmap_get(&shadow_owner, class_getName(c), &v)) return (struct gclass *)(uintptr_t)v;
    }
    return NULL;
}

gaddr_t shadow_guest(id o) {
    if (!o) return 0;
    Class cls = object_getClass(o);
    Ivar iv = class_getInstanceVariable(cls, SHADOW_IVAR);
    if (!iv || !gclass_of_shadow(cls)) return 0;
    return *(uint32_t *)((uint8_t *)(__bridge void *)o + ivar_getOffset(iv));
}

static void shadow_set_guest(id o, gaddr_t g) {
    Ivar iv = class_getInstanceVariable(object_getClass(o), SHADOW_IVAR);
    *(uint32_t *)((uint8_t *)(__bridge void *)o + ivar_getOffset(iv)) = g;
}

// Host types for a selector a guest class implements: prefer the host
// ancestor's declaration (overrides), then the generated signature table,
// then derive from the guest's own i386 types.
static const char *host_types_for(struct gclass *c, SEL sel, bool class_method, gaddr_t gtypes) {
    Class anc = class_method ? object_getClass(c->host_ancestor) : c->host_ancestor;
    Method m = class_method ? class_getClassMethod(c->host_ancestor, sel) : class_getInstanceMethod(anc, sel);
    if (m) return method_getTypeEncoding(m);
    // Protocol methods the host might call (delegates) are declared by the host
    // frameworks; scan the protocols the host knows.
    unsigned int np = 0;
    Protocol *__unsafe_unretained *protos = objc_copyProtocolList(&np);
    const char *found = NULL;
    for (unsigned int i = 0; i < np && !found; i++) {
        for (int req = 0; req < 2 && !found; req++) {
            struct objc_method_description d = protocol_getMethodDescription(protos[i], sel, req, !class_method);
            if (d.name) found = d.types;
        }
    }
    free(protos);
    if (found) return found;
    return host_types_from_guest(g2h(gtypes));
}

static void shadow_forward_invocation(id self, SEL _cmd, NSInvocation *inv);
static void shadow_dealloc(id self, SEL _cmd);

static void add_forwarders(struct gclass *c, Class shadow, bool class_method) {
    struct gmethod *ms = class_method ? c->cmethods : c->imethods;
    int n = class_method ? c->ncmethods : c->nimethods;
    Class target = class_method ? object_getClass(shadow) : shadow;
    for (int i = 0; i < n; i++) {
        const char *name = gsel_name(ms[i].sel);
        if (!class_method && strcmp(name, "dealloc") == 0) continue; // handled by shadow_dealloc
        SEL sel = sel_registerName(name);
        const char *ht = host_types_for(c, sel, class_method, ms[i].types);
        IMP fwd = _objc_msgForward;
#if defined(__x86_64__)
        // Methods returning large structs use the stret forwarding entry on x86_64.
        if (ht && ht[0] == '{' && guest_type_size(ht) > 16) fwd = (IMP)_objc_msgForward_stret;
#endif
        class_addMethod(target, sel, fwd, ht);
    }
}

Class gclass_shadow(struct gclass *c) {
    pthread_mutex_lock(&conv_lock);
    if (c->shadow) {
        pthread_mutex_unlock(&conv_lock);
        return c->shadow;
    }
    if (!shadow_owner.slots) strmap_init(&shadow_owner);
    Class super = c->super_g ? gclass_shadow(c->super_g) : c->host_ancestor;
    char name[256];
    snprintf(name, sizeof name, "DXG_%s", c->name);
    Class s = objc_allocateClassPair(super, name, 0);
    if (!s) fatal("objc: cannot create shadow class %s", name);
    if (!c->super_g) {
        class_addIvar(s, SHADOW_IVAR, sizeof(uint32_t), 2, "I");
        class_addMethod(s, @selector(forwardInvocation:), (IMP)shadow_forward_invocation, "v@:@");
        class_addMethod(object_getClass(s), @selector(forwardInvocation:), (IMP)shadow_forward_invocation, "v@:@");
        class_addMethod(s, @selector(dealloc), (IMP)shadow_dealloc, "v@:");
    }
    objc_registerClassPair(s);
    strmap_put(&shadow_owner, name, (uint64_t)(uintptr_t)c, false);
    c->shadow = s;
    add_forwarders(c, s, false);
    add_forwarders(c, s, true);
    pthread_mutex_unlock(&conv_lock);
    LOGD("objc: shadow class %s : %s", name, class_getName(super));
    return s;
}

gaddr_t gobj_alloc_instance(struct gclass *c) {
    Class s = gclass_shadow(c);
    id host = ((id(*)(id, SEL))objc_msgSend)((id)s, @selector(alloc));
    if (!host) return 0;
    gaddr_t g = gheap_calloc(1, c->instance_size < 4 ? 4 : c->instance_size);
    gwrite32(g, c->cls);
    shadow_set_guest(host, g);
    pthread_mutex_lock(&conv_lock);
    inst_put(g, (__bridge void *)host);
    pthread_mutex_unlock(&conv_lock);
    LOGT("objc: alloc %s -> guest %#x host %p", c->name, g, (__bridge void *)host);
    return g;
}

// Guest -dealloc implementations end with [super dealloc]; the bridge then runs
// the host superclass dealloc and marks the object (see objc_msg.m).
_Thread_local id shadow_dealloc_in_progress;
_Thread_local bool shadow_host_dealloc_done;

static void shadow_dealloc(id self, SEL _cmd) {
    gaddr_t g = shadow_guest(self);
    struct gclass *c = gclass_of_shadow(object_getClass(self));
    const struct gmethod *gm = c ? gclass_find_method(c, gsel_unique("dealloc"), false) : NULL;
    id prev = shadow_dealloc_in_progress;
    bool prev_done = shadow_host_dealloc_done;
    shadow_dealloc_in_progress = self;
    shadow_host_dealloc_done = false;
    if (gm) {
        uint32_t args[2] = {g, gm->sel};
        cpu_call(gm->imp, 2, args);
    }
    if (!shadow_host_dealloc_done) {
        // Guest has no -dealloc (or did not call super): run the host one.
        Class top = object_getClass(self);
        while (gclass_of_shadow(class_getSuperclass(top))) top = class_getSuperclass(top);
        struct objc_super sup = {self, class_getSuperclass(top)};
        ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(dealloc));
    }
    shadow_dealloc_in_progress = prev;
    shadow_host_dealloc_done = prev_done;
    if (g) {
        guest_instance_forget(g);
        gheap_free(g);
    }
}

static void shadow_forward_invocation(id self, SEL _cmd, NSInvocation *inv) {
    bool is_class = object_isClass(self);
    Class cls = is_class ? (Class)self : object_getClass(self);
    struct gclass *c = gclass_of_shadow(cls);
    SEL sel = inv.selector;
    gaddr_t gsel = host_sel_to_guest(sel);
    const struct gmethod *gm = c ? gclass_find_method(c, gsel, is_class) : NULL;
    if (!gm) {
        LOGE("objc: shadow %s cannot forward %s", class_getName(cls), sel_getName(sel));
        [self doesNotRecognizeSelector:sel];
        return;
    }
    gaddr_t gself = is_class ? c->cls : shadow_guest(self);
    cpu_thread_attach();
    marshal_invocation_to_guest(inv, gself, gm->imp, gsel, g2h(gm->types));
}

// --- conversions -------------------------------------------------------------------------

id g2o(gaddr_t g) {
    if (!g) return nil;
    if (hobj_is_handle(g)) return (__bridge id)hobj_unwrap(g);
    struct gclass *c = gclass_by_addr(g);
    if (c) {
        Class s = gclass_shadow(c);
        return g == c->meta ? (id)object_getClass(s) : (id)s;
    }
    id h = guest_instance_host(g);
    if (h) return h;
    // Guest blocks used as objects
    gaddr_t isa = gmem_is_mapped(g, 4) ? gread32(g) : 0;
    if (isa && (isa == hostsym_lookup("__NSConcreteStackBlock") || isa == hostsym_lookup("__NSConcreteGlobalBlock") ||
                isa == hostsym_lookup("__NSConcreteMallocBlock"))) {
        extern id guest_block_to_host(gaddr_t);
        return guest_block_to_host(g);
    }
    return (__bridge id)gcf(g); // constant CFStrings etc.
}

gaddr_t o2g(id o) {
    if (!o) return 0;
    if (object_isClass(o)) {
        struct gclass *c = gclass_of_shadow((Class)o);
        if (c && c->shadow == (Class)o) return c->cls;
        if (class_isMetaClass((Class)o)) {
            for (Class k = (Class)o; k; k = class_getSuperclass(k)) {}
        }
        return hobj_wrap((__bridge const void *)o);
    }
    gaddr_t g = shadow_guest(o);
    if (g) return g;
    return cfg((__bridge CFTypeRef)o);
}

static CFTypeRef foreign_resolver(gaddr_t g) {
    if (gclass_by_addr(g)) return (__bridge CFTypeRef)g2o(g);
    return (__bridge CFTypeRef)guest_instance_host(g);
}

__attribute__((constructor)) static void install_cf_hooks(void) { cf_bridge_set_foreign_resolver(foreign_resolver); }
