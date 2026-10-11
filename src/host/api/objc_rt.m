// Guest Objective-C runtime data: selector uniquing, class registration from
// the legacy __OBJC segment, method lookup, class references.
#import "objc_internal.h"

#import <Foundation/Foundation.h>
#include "../macho.h"
#include "../runtime.h"
#include "../strmap.h"

#include <pthread.h>

static pthread_mutex_t rt_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static struct strmap sel_by_name;   // name -> guest SEL
static struct strmap class_by_name; // name -> struct gclass*

// guest class/meta address -> struct gclass* (small open-addressing map)
static struct { gaddr_t key; struct gclass *val; } *cls_map;
static uint32_t cls_cap, cls_count;

static void cls_put(gaddr_t k, struct gclass *v) {
    if ((cls_count + 1) * 2 > cls_cap) {
        uint32_t oc = cls_cap;
        __typeof__(cls_map) old = cls_map;
        cls_cap = cls_cap ? cls_cap * 2 : 256;
        cls_map = calloc(cls_cap, sizeof *cls_map);
        cls_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i].key) cls_put(old[i].key, old[i].val);
        free(old);
    }
    uint32_t j = (k * 2654435761u) & (cls_cap - 1);
    while (cls_map[j].key && cls_map[j].key != k) j = (j + 1) & (cls_cap - 1);
    if (!cls_map[j].key) cls_count++;
    cls_map[j].key = k;
    cls_map[j].val = v;
}

struct gclass *gclass_by_addr(gaddr_t a) {
    if (!cls_cap || !a) return NULL;
    uint32_t j = (a * 2654435761u) & (cls_cap - 1);
    while (cls_map[j].key) {
        if (cls_map[j].key == a) return cls_map[j].val;
        j = (j + 1) & (cls_cap - 1);
    }
    return NULL;
}

bool gaddr_is_gclass_meta(gaddr_t a) {
    struct gclass *c = gclass_by_addr(a);
    return c && c->meta == a;
}

struct gclass *gclass_by_name(const char *name) {
    uint64_t v;
    return strmap_get(&class_by_name, name, &v) ? (struct gclass *)(uintptr_t)v : NULL;
}

gaddr_t gsel_unique(const char *name) {
    pthread_mutex_lock(&rt_lock);
    if (!sel_by_name.slots) strmap_init(&sel_by_name);
    uint64_t v;
    gaddr_t s;
    if (strmap_get(&sel_by_name, name, &v)) s = (gaddr_t)v;
    else {
        s = gheap_strdup(name);
        strmap_put(&sel_by_name, name, s, false);
    }
    pthread_mutex_unlock(&rt_lock);
    return s;
}

const char *gsel_name(gaddr_t sel) { return sel ? (const char *)g2h(sel) : "(null)"; }

SEL gsel_to_host(gaddr_t sel) { return sel ? sel_registerName(gsel_name(sel)) : NULL; }

gaddr_t host_sel_to_guest(SEL sel) { return sel ? gsel_unique(sel_getName(sel)) : 0; }

gaddr_t host_class_handle(Class c) { return c ? hobj_wrap((__bridge const void *)c) : 0; }

// --- method lists -------------------------------------------------------------------

static void add_methods(struct gmethod **list, int *n, int *cap, gaddr_t mlist, bool prepend) {
    if (!mlist || mlist == 0xFFFFFFFFu) return;
    uint32_t count = gread32(mlist + 4);
    for (uint32_t i = 0; i < count; i++) {
        gaddr_t m = mlist + 8 + 12 * i;
        gaddr_t name = gread32(m);
        if (!name) continue;
        // Unique the selector in place so guest code comparing SELs agrees.
        gaddr_t sel = gsel_unique(g2h(name));
        gwrite32(m, sel);
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 16;
            *list = realloc(*list, sizeof **list * (size_t)*cap);
        }
        struct gmethod gm = {sel, gread32(m + 4), gread32(m + 8)};
        if (prepend) { // categories override class methods
            memmove(*list + 1, *list, sizeof **list * (size_t)*n);
            (*list)[0] = gm;
        } else {
            (*list)[*n] = gm;
        }
        (*n)++;
    }
}

static void add_method_lists(struct gmethod **list, int *n, int *cap, gaddr_t cls) {
    gaddr_t ml = gread32(cls + GCLS_METHODS);
    if (!ml) return;
    // Compiled classes point directly at one method list unless CLS_METHOD_ARRAY is set.
    if (!(gread32(cls + GCLS_INFO) & CLS_METHOD_ARRAY)) {
        add_methods(list, n, cap, ml, false);
    } else {
        for (int i = 0;; i++) {
            gaddr_t l = gread32(ml + 4 * (uint32_t)i);
            if (!l || l == 0xFFFFFFFFu) break;
            add_methods(list, n, cap, l, false);
        }
    }
}

const struct gmethod *gclass_find_method(struct gclass *c, gaddr_t sel, bool class_method) {
    for (; c; c = c->super_g) {
        struct gmethod *m = class_method ? c->cmethods : c->imethods;
        int n = class_method ? c->ncmethods : c->nimethods;
        for (int i = 0; i < n; i++)
            if (m[i].sel == sel) return &m[i];
    }
    return NULL;
}

// Guest categories on *host* classes: class name -> methods.
struct hostcat {
    char *class_name;
    struct gmethod *imethods, *cmethods;
    int ni, nc, ci, cc;
    struct hostcat *next;
};
static struct hostcat *host_categories;

const struct gmethod *hostcat_find(Class cls, gaddr_t sel, bool class_method) {
    if (!host_categories) return NULL;
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        const char *cn = class_getName(c);
        for (struct hostcat *h = host_categories; h; h = h->next) {
            if (strcmp(h->class_name, cn) != 0) continue;
            struct gmethod *m = class_method ? h->cmethods : h->imethods;
            int n = class_method ? h->nc : h->ni;
            for (int i = 0; i < n; i++)
                if (m[i].sel == sel) return &m[i];
        }
    }
    return NULL;
}

// --- registration --------------------------------------------------------------------

static Class host_class_named(const char *name) {
    Class c = objc_getClass(name);
    if (!c) LOGI("objc: host class %s does not exist on this system", name);
    return c;
}

static void register_class(gaddr_t cls) {
    gaddr_t meta = gread32(cls + GCLS_ISA);
    const char *name = g2h(gread32(cls + GCLS_NAME));
    struct gclass *c = calloc(1, sizeof *c);
    c->cls = cls;
    c->meta = meta;
    c->name = strdup(name);
    c->instance_size = gread32(cls + GCLS_INSTANCE_SIZE);
    add_method_lists(&c->imethods, &c->nimethods, &c->cap_i, cls);
    add_method_lists(&c->cmethods, &c->ncmethods, &c->cap_c, meta);
    strmap_put(&class_by_name, name, (uint64_t)(uintptr_t)c, false);
    cls_put(cls, c);
    cls_put(meta, c);
    LOGD("objc: guest class %s (%d instance, %d class methods, size %u)", name, c->nimethods, c->ncmethods,
         c->instance_size);
}

static void resolve_supers(void) {
    for (size_t i = 0; i < class_by_name.cap; i++) {
        if (!class_by_name.slots[i].key) continue;
        struct gclass *c = (struct gclass *)(uintptr_t)class_by_name.slots[i].value;
        gaddr_t sup_field = gread32(c->cls + GCLS_SUPER);
        if (!sup_field || gclass_by_addr(sup_field)) continue; // already resolved
        const char *sup_name = g2h(sup_field);
        struct gclass *sg = gclass_by_name(sup_name);
        if (sg) {
            c->super_g = sg;
            gwrite32(c->cls + GCLS_SUPER, sg->cls);
            gwrite32(c->meta + GCLS_SUPER, sg->meta);
        } else {
            Class hc = host_class_named(sup_name);
            c->host_ancestor = hc;
            gwrite32(c->cls + GCLS_SUPER, host_class_handle(hc));
            gwrite32(c->meta + GCLS_SUPER, hc ? host_class_handle(object_getClass(hc)) : 0);
        }
        // metaclass isa points at the root metaclass (by name in the file)
        gaddr_t root_name = gread32(c->meta + GCLS_ISA);
        if (root_name && !gclass_by_addr(root_name)) {
            Class root = host_class_named(g2h(root_name));
            gwrite32(c->meta + GCLS_ISA, root ? host_class_handle(object_getClass(root)) : 0);
        }
    }
    // Propagate host ancestors down guest-only chains.
    for (size_t i = 0; i < class_by_name.cap; i++) {
        if (!class_by_name.slots[i].key) continue;
        struct gclass *c = (struct gclass *)(uintptr_t)class_by_name.slots[i].value;
        struct gclass *p = c;
        while (p->super_g) p = p->super_g;
        c->host_ancestor = p->host_ancestor ? p->host_ancestor : [NSObject class];
    }
}

static void register_category(gaddr_t cat) {
    const char *cname = g2h(gread32(cat + 4));
    gaddr_t im = gread32(cat + 8), cm = gread32(cat + 12);
    struct gclass *gc = gclass_by_name(cname);
    if (gc) {
        add_methods(&gc->imethods, &gc->nimethods, &gc->cap_i, im, true);
        add_methods(&gc->cmethods, &gc->ncmethods, &gc->cap_c, cm, true);
        LOGD("objc: category %s on guest class %s", (const char *)g2h(gread32(cat)), cname);
        return;
    }
    struct hostcat *h = calloc(1, sizeof *h);
    h->class_name = strdup(cname);
    add_methods(&h->imethods, &h->ni, &h->ci, im, false);
    add_methods(&h->cmethods, &h->nc, &h->cc, cm, false);
    h->next = host_categories;
    host_categories = h;
    LOGI("objc: guest category %s on host class %s (%d/%d methods)", (const char *)g2h(gread32(cat)), cname, h->ni,
         h->nc);
}

static void fix_refs(struct image *img) {
    const struct image_section *s = macho_section(img, "__OBJC", "__message_refs");
    if (s)
        for (uint32_t i = 0; i < s->size / 4; i++) {
            gaddr_t p = gread32(s->addr + 4 * i);
            if (p) gwrite32(s->addr + 4 * i, gsel_unique(g2h(p)));
        }
    s = macho_section(img, "__OBJC", "__cls_refs");
    if (s)
        for (uint32_t i = 0; i < s->size / 4; i++) {
            gaddr_t p = gread32(s->addr + 4 * i);
            if (!p || gclass_by_addr(p) || hobj_is_handle(p)) continue;
            const char *name = g2h(p);
            struct gclass *gc = gclass_by_name(name);
            gwrite32(s->addr + 4 * i, gc ? gc->cls : host_class_handle(host_class_named(name)));
        }
}

static void register_image(struct image *img) {
    const struct image_section *mod = macho_section(img, "__OBJC", "__module_info");
    if (!mod) return;
    gaddr_t cats[256];
    int ncats = 0;
    for (uint32_t off = 0; off + 16 <= mod->size; off += 16) {
        gaddr_t symtab = gread32(mod->addr + off + 12);
        if (!symtab) continue;
        uint16_t ncls = gread16(symtab + 8), ncat = gread16(symtab + 10);
        for (uint16_t i = 0; i < ncls; i++) register_class(gread32(symtab + 12 + 4 * i));
        for (uint16_t i = 0; i < ncat && ncats < 256; i++) cats[ncats++] = gread32(symtab + 12 + 4 * (ncls + i));
    }
    resolve_supers();
    for (int i = 0; i < ncats; i++) register_category(cats[i]);
    fix_refs(img);
    LOGI("objc: registered Objective-C metadata of %s", img->path);
}

void objc_bridge_register_image(struct image *img) {
    pthread_mutex_lock(&rt_lock);
    if (!class_by_name.slots) strmap_init(&class_by_name);
    if (!sel_by_name.slots) strmap_init(&sel_by_name);
    register_image(img);
    pthread_mutex_unlock(&rt_lock);
}

void objc_bridge_register_images(void) {
    for (int i = 0; i < runtime_image_count(); i++) objc_bridge_register_image(runtime_image(i));
}

// --- class lookup thunks ----------------------------------------------------------------

gaddr_t gclass_lookup_name(const char *name) {
    struct gclass *gc = gclass_by_name(name);
    if (gc) return gc->cls;
    Class c = objc_getClass(name);
    return c ? host_class_handle(c) : 0;
}

THUNK(objc_getClass) { RET(gclass_lookup_name(ARGS(0))); }
THUNK(objc_lookUpClass) { RET(gclass_lookup_name(ARGS(0))); }
THUNK(objc_getMetaClass) {
    struct gclass *gc = gclass_by_name(ARGS(0));
    if (gc) RET(gc->meta);
    Class c = objc_getClass(ARGS(0));
    RET(c ? host_class_handle(object_getClass(c)) : 0);
}
THUNK(sel_registerName) { RET(gsel_unique(ARGS(0))); }
THUNK(sel_getUid) { RET(gsel_unique(ARGS(0))); }
THUNK(sel_getName) { RET(ARG(0)); }
THUNK(NSClassFromString) {
    NSString *s = g2o(ARG(0));
    RET(s ? gclass_lookup_name(s.UTF8String) : 0);
}
THUNK(NSStringFromClass) {
    struct gclass *gc = gclass_by_addr(ARG(0));
    NSString *s = gc ? @(gc->name) : NSStringFromClass((Class)hobj_unwrap(ARG(0)));
    RET(o2g(s));
}
THUNK(NSSelectorFromString) {
    NSString *s = g2o(ARG(0));
    RET(s ? gsel_unique(s.UTF8String) : 0);
}
THUNK(NSStringFromSelector) { RET(ARG(0) ? o2g(@(gsel_name(ARG(0)))) : 0); }
