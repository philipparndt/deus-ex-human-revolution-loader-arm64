// objc_msgSend family, legacy exceptions, properties, synchronisation and
// fast enumeration for guest code.
#import "objc_internal.h"
#include "mainq.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/objc-sync.h>
#include "blocks.h"
#include "objc_sigs.h"

#include <pthread.h>

extern const struct gmethod *hostcat_find(Class cls, gaddr_t sel, bool class_method);
extern _Thread_local id shadow_dealloc_in_progress;
extern _Thread_local bool shadow_host_dealloc_done;

// --- signatures -------------------------------------------------------------------------

const char *objc_guest_types_for(Class cls, SEL sel, bool class_method) {
    const char *name = sel_getName(sel);
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        Class k = class_isMetaClass(c) ? objc_getClass(class_getName(c)) : c;
        const struct objc_sig *s = objc_sig_lookup(class_getName(k), name, class_method);
        if (s && s->i386_types) return s->i386_types;
    }
    const struct objc_sig *s = objc_sig_lookup_any(name, class_method);
    return s ? s->i386_types : NULL;
}

// --- legacy exceptions --------------------------------------------------------------------
// struct _objc_exception_data { int buf[18]; void *pointers[4]; } -- buf holds
// our setjmp layout (the compiler calls _setjmp on it right after try_enter).
#define EXC_POINTERS 72

static _Thread_local gaddr_t exc_stack[128];
static _Thread_local int exc_depth;
static gaddr_t uncaught_handler;

THUNK(objc_exception_try_enter) {
    if (exc_depth < 128) exc_stack[exc_depth++] = ARG(0);
}
THUNK(objc_exception_try_exit) {
    if (exc_depth > 0 && exc_stack[exc_depth - 1] == ARG(0)) exc_depth--;
}
THUNK(objc_exception_extract) { RET(gread32(ARG(0) + EXC_POINTERS)); }
THUNK(objc_exception_match) {
    id cls = g2o(ARG(0)), exc = g2o(ARG(1));
    RET(exc && cls && [exc isKindOfClass:cls]);
}

// Unwind to the innermost guest @try (longjmp through the guest registers).
static void guest_throw(struct guest_regs *r, gaddr_t exc) {
    if (exc_depth == 0) {
        id e = g2o(exc);
        LOGE("objc: uncaught exception %s", [[e description] UTF8String] ?: "?");
        if (uncaught_handler) gcall1(uncaught_handler, exc);
        fatal("objc: uncaught Objective-C exception");
    }
    gaddr_t d = exc_stack[--exc_depth];
    gwrite32(d + EXC_POINTERS, exc);
    // Same as longjmp(d->buf, 1); see setjmp.c for the buffer layout.
    gaddr_t jb = d;
    r->ebx = gread32(jb);
    r->esi = gread32(jb + 4);
    r->edi = gread32(jb + 8);
    r->ebp = gread32(jb + 12);
    r->esp = gread32(jb + 16) - 4;
    gwrite32(r->esp, gread32(jb + 20));
    r->pop = 0;
    r->fp_ret = 0;
    r->ret = 1;
}

THUNK(objc_exception_throw) { guest_throw(r, ARG(0)); }
THUNK(NSSetUncaughtExceptionHandler) { uncaught_handler = ARG(0); }
THUNK(NSGetUncaughtExceptionHandler) { RET(uncaught_handler); }

// --- dispatch helpers -------------------------------------------------------------------------

// Jump to a guest IMP with the caller's original stack (tail call).
static void tail_call(struct guest_regs *r, gaddr_t imp) {
    r->esp -= 4;
    gwrite32(r->esp, imp);
    r->pop = 0;
}

static gaddr_t sel_alloc, sel_allocWithZone, sel_new, sel_init, sel_class, sel_dealloc, sel_self;

static void init_sels(void) {
    if (sel_alloc) return;
    sel_alloc = gsel_unique("alloc");
    sel_allocWithZone = gsel_unique("allocWithZone:");
    sel_new = gsel_unique("new");
    sel_init = gsel_unique("init");
    sel_class = gsel_unique("class");
    sel_dealloc = gsel_unique("dealloc");
    sel_self = gsel_unique("self");
}

static void update_nsapp(void);

// Call a host method for the guest. args_at = guest address of the first
// argument after _cmd; stret = hidden struct-return pointer or 0.
static void call_host(struct guest_regs *r, id recv, Class lookup_cls, SEL sel, IMP imp, gaddr_t args_at,
                      gaddr_t stret, bool class_method);

static bool special_format_selector(SEL sel);

// Methods that must run on the main thread on current macOS.
static bool needs_main_thread(id recv, SEL sel) {
    static Class glctx, view, window, app;
    if (!glctx) {
        glctx = [NSOpenGLContext class];
        view = [NSView class];
        window = [NSWindow class];
        app = [NSApplication class];
    }
    if (object_isClass(recv)) return false;
    if ([recv isKindOfClass:glctx])
        return sel == @selector(update) || sel == @selector(setView:) || sel == @selector(clearDrawable) ||
               sel == @selector(view);
    return [recv isKindOfClass:view] || [recv isKindOfClass:window] || [recv isKindOfClass:app];
}
static void call_format_method(struct guest_regs *r, id recv, SEL sel, gaddr_t args_at, Class lookup);
static void fast_enumerate(struct guest_regs *r, id recv, gaddr_t args_at);

// Core of objc_msgSend / _stret / _fpret. arg0 = index of the receiver argument.
static void msg_send(struct guest_regs *r, int arg0) {
    init_sels();
    gaddr_t recv = ARG(arg0), gsel = ARG(arg0 + 1);
    gaddr_t stret = arg0 ? ARG(0) : 0;
    if (arg0) r->pop = 4; // _stret callee pops the hidden pointer
    if (!recv) {
        r->ret = 0;
        r->fret = 0;
        r->fp_ret = 1;
        return;
    }
    gaddr_t args_at = r->esp + 4 + 4 * (uint32_t)(arg0 + 2);

    // Guest class (class method)?
    struct gclass *gc = gclass_by_addr(recv);
    if (gc && gc->cls == recv) {
        const struct gmethod *m = gclass_find_method(gc, gsel, true);
        if (m) return tail_call(r, m->imp);
        if (gsel == sel_alloc || gsel == sel_allocWithZone) {
            r->ret = gobj_alloc_instance(gc);
            return;
        }
        if (gsel == sel_new) {
            gaddr_t o = gobj_alloc_instance(gc);
            const struct gmethod *im = gclass_find_method(gc, sel_init, false);
            if (im) o = gcall2(im->imp, o, sel_init);
            else [g2o(o) init];
            r->ret = o;
            return;
        }
        if (gsel == sel_class || gsel == sel_self) {
            r->ret = recv;
            return;
        }
        Class shadow = gclass_shadow(gc);
        SEL hs = gsel_to_host(gsel);
        call_host(r, (id)shadow, object_getClass(shadow), hs, NULL, args_at, stret, true);
        return;
    }

    // Guest instance?
    if (!hobj_is_handle(recv) && gmem_is_mapped(recv, 4)) {
        struct gclass *ic = gclass_by_addr(gread32(recv));
        if (ic && ic->cls == gread32(recv)) {
            const struct gmethod *m = gclass_find_method(ic, gsel, false);
            if (m) return tail_call(r, m->imp);
            if (gsel == sel_class) {
                r->ret = ic->cls;
                return;
            }
            id host = guest_instance_host(recv);
            if (!host) fatal("objc: guest object %#x (%s) has no host peer", recv, ic->name);
            SEL hs = gsel_to_host(gsel);
            call_host(r, host, object_getClass(host), hs, NULL, args_at, stret, false);
            return;
        }
    }

    // Host object (handle, constant string, ...)
    id obj = g2o(recv);
    if (!obj) {
        LOGE("objc: message %s to unknown object %#x", gsel_name(gsel), recv);
        r->ret = 0;
        return;
    }
    bool is_class = object_isClass(obj);
    Class cls = object_getClass(obj);
    const struct gmethod *cat = hostcat_find(is_class ? (Class)obj : cls, gsel, is_class);
    if (cat) return tail_call(r, cat->imp);
    SEL hs = gsel_to_host(gsel);
    call_host(r, obj, cls, hs, NULL, args_at, stret, is_class);
}

// (const id *objects, [const id *keys,] NSUInteger count) constructors: the generic
// marshaller would convert only the first element of the C array.
static bool call_counted_object_method(struct guest_regs *r, id recv, SEL sel, gaddr_t args_at) {
    static SEL one[4], two[2];
    if (!one[0]) {
        one[0] = @selector(arrayWithObjects:count:);
        one[1] = @selector(initWithObjects:count:);
        one[2] = @selector(setWithObjects:count:);
        one[3] = @selector(orderedSetWithObjects:count:);
        two[0] = @selector(dictionaryWithObjects:forKeys:count:);
        two[1] = @selector(initWithObjects:forKeys:count:);
    }
    int arrays = 0;
    for (int i = 0; i < 4; i++)
        if (sel == one[i]) arrays = 1;
    for (int i = 0; i < 2; i++)
        if (sel == two[i]) arrays = 2;
    if (!arrays) return false;
    uint32_t n = gread32(args_at + 4 * arrays);
    id *bufs[2] = {NULL, NULL};
    for (int a = 0; a < arrays; a++) {
        gaddr_t g = gread32(args_at + 4 * a);
        bufs[a] = calloc(n ? n : 1, sizeof(id));
        for (uint32_t i = 0; i < n; i++) bufs[a][i] = g2o(gread32(g + 4 * i));
    }
    id res;
    @try {
        if (arrays == 1) res = ((id (*)(id, SEL, id *, NSUInteger))objc_msgSend)(recv, sel, bufs[0], n);
        else res = ((id (*)(id, SEL, id *, id *, NSUInteger))objc_msgSend)(recv, sel, bufs[0], bufs[1], n);
    } @catch (NSException *e) {
        free(bufs[0]);
        free(bufs[1]);
        LOGI("objc: host exception %s in %s: %s", e.name.UTF8String, sel_getName(sel), e.reason.UTF8String ?: "");
        [e retain];
        guest_throw(r, o2g(e));
        return true;
    }
    free(bufs[0]);
    free(bufs[1]);
    r->ret = o2g(res);
    return true;
}

// Variadic nil-terminated object lists (arrayWithObjects: etc.).
static bool call_object_list_method(struct guest_regs *r, id recv, SEL sel, gaddr_t args_at) {
    static SEL sels[8];
    if (!sels[0]) {
        sels[0] = @selector(arrayWithObjects:);
        sels[1] = @selector(initWithObjects:);
        sels[2] = @selector(setWithObjects:);
        sels[3] = @selector(dictionaryWithObjectsAndKeys:);
        sels[4] = @selector(initWithObjectsAndKeys:);
        sels[5] = @selector(orderedSetWithObjects:);
    }
    int which = -1;
    for (int i = 0; i < 6; i++)
        if (sel == sels[i]) which = i;
    if (which < 0) return false;
    NSMutableArray *items = [NSMutableArray array];
    for (gaddr_t p = args_at;; p += 4) {
        gaddr_t g = gread32(p);
        if (!g) break;
        [items addObject:g2o(g)];
    }
    id res = nil;
    switch (which) {
    case 0: res = [(Class)recv arrayWithArray:items]; break;
    case 1: res = [recv initWithArray:items]; break;
    case 2: res = [(Class)recv setWithArray:items]; break;
    case 5: res = [(Class)recv orderedSetWithArray:items]; break;
    case 3:
    case 4: {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        for (NSUInteger i = 0; i + 1 < items.count; i += 2) d[items[i + 1]] = items[i];
        res = which == 3 ? [(Class)recv dictionaryWithDictionary:d] : [recv initWithDictionary:d];
        break;
    }
    }
    r->ret = o2g(res);
    return true;
}

static void call_host(struct guest_regs *r, id recv, Class lookup_cls, SEL sel, IMP imp, gaddr_t args_at,
                      gaddr_t stret, bool class_method) {
    if (call_object_list_method(r, recv, sel, args_at)) return;
    if (call_counted_object_method(r, recv, sel, args_at)) return;
    if (special_format_selector(sel)) return call_format_method(r, recv, sel, args_at, lookup_cls);
    if (sel == @selector(countByEnumeratingWithState:objects:count:)) return fast_enumerate(r, recv, args_at);
    Method m = class_getInstanceMethod(lookup_cls, sel);
    const char *htypes = m ? method_getTypeEncoding(m) : NULL;
    if (!imp) imp = m ? method_getImplementation(m) : NULL;
    if (!htypes) {
        // Not implemented directly: ask the object (forwarding proxies etc.).
        NSMethodSignature *sig = nil;
        @try {
            sig = [recv methodSignatureForSelector:sel];
        } @catch (NSException *e) {
        }
        if (!sig) {
            LOGE("objc: %s does not respond to %c%s", class_getName(lookup_cls), class_method ? '+' : '-',
                 sel_getName(sel));
            r->ret = 0;
            return;
        }
        static char buf[1024];
        snprintf(buf, sizeof buf, "%s@:", sig.methodReturnType);
        for (NSUInteger i = 2; i < sig.numberOfArguments; i++)
            strlcat(buf, [sig getArgumentTypeAtIndex:i], sizeof buf);
        htypes = strdup(buf);
        imp = (IMP)objc_msgSend;
    }
    struct mcall mc = {.args_at = args_at,
                       .gtypes = objc_guest_types_for(class_method ? (Class)recv : lookup_cls, sel, class_method),
                       .htypes = htypes,
                       .stret_ptr = stret};
    LOGT("objc: %c[%s %s] host %s guest %s", class_method ? '+' : '-', class_getName(lookup_cls), sel_getName(sel),
         htypes, mc.gtypes ? mc.gtypes : "(derived)");
    @try {
        if (needs_main_thread(recv, sel) && !pthread_main_np()) {
            // Modern AppKit traps when views/windows (and NSOpenGLContext's
            // view updates) are touched off the main thread; 2014 code did that.
            __block NSException *exc = nil;
            extern void cgl_drop_held_locks(void), cgl_retake_held_locks(void);
            cgl_drop_held_locks(); // the main thread may be waiting for them
            mainq_run(^{
                @try {
                    marshal_call_host(recv, sel, imp, &mc);
                } @catch (NSException *e) {
                    exc = [e retain];
                }
            });
            cgl_retake_held_locks();
            if (exc) @throw [exc autorelease];
        } else {
            marshal_call_host(recv, sel, imp, &mc);
        }
    } @catch (NSException *e) {
        LOGI("objc: host exception %s in %s: %s", e.name.UTF8String, sel_getName(sel), e.reason.UTF8String ?: "");
        [e retain];
        return guest_throw(r, o2g(e));
    }
    r->ret = mc.ret;
    LOGT("objc:   = %#x", (uint32_t)mc.ret);
    if (sel == @selector(copyNextSampleBuffer) && !mc.ret) {
        // NULL from an asset reader output: report why, once per output
        static NSHashTable *seen;
        static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&seen_lock);
        if (!seen) seen = [[NSHashTable weakObjectsHashTable] retain];
        bool first = ![seen containsObject:recv];
        if (first) [seen addObject:recv];
        pthread_mutex_unlock(&seen_lock);
        if (first) {
            id track = [recv respondsToSelector:@selector(track)] ? [recv performSelector:@selector(track)] : nil;
            LOGI("objc: copyNextSampleBuffer returned NULL on %s output (track %s)", class_getName([recv class]),
                 [[track description] UTF8String] ?: "?");
        }
    }
    if (mc.fp_ret) {
        r->fret = mc.fret;
        r->fp_ret = 1;
    }
    update_nsapp();
}

THUNK(objc_msgSend) { msg_send(r, 0); }
THUNK(objc_msgSend_fpret) { msg_send(r, 0); }
THUNK(objc_msgSend_stret) { msg_send(r, 1); }

// objc_msgSendSuper(struct objc_super *super, SEL op, ...): super = {receiver, class}
static void msg_send_super(struct guest_regs *r, int arg0) {
    init_sels();
    gaddr_t sup = ARG(arg0), gsel = ARG(arg0 + 1);
    gaddr_t stret = arg0 ? ARG(0) : 0;
    if (arg0) r->pop = 4;
    gaddr_t recv = gread32(sup), cls = gread32(sup + 4);
    gaddr_t args_at = r->esp + 4 + 4 * (uint32_t)(arg0 + 2);
    struct gclass *gc = gclass_by_addr(cls);
    if (gc) {
        bool meta = gc->meta == cls;
        const struct gmethod *m = gclass_find_method(gc, gsel, meta);
        if (m) {
            gwrite32(r->esp + 4 + 4 * (uint32_t)arg0, recv); // replace objc_super* by the receiver
            return tail_call(r, m->imp);
        }
        // Continue with the host ancestor.
        Class anc = gc->host_ancestor;
        cls = host_class_handle(meta ? object_getClass(anc) : anc);
    }
    Class super_cls = (Class)hobj_unwrap(cls);
    if (!super_cls) fatal("objc: [super %s] with unknown class %#x", gsel_name(gsel), cls);
    bool class_method = class_isMetaClass(super_cls);
    struct gclass *rc = gclass_by_addr(recv);
    if (class_method && rc && (gsel == sel_alloc || gsel == sel_allocWithZone)) {
        r->ret = gobj_alloc_instance(rc);
        return;
    }
    id host = g2o(recv);
    SEL hs = gsel_to_host(gsel);
    Method m = class_method ? class_getInstanceMethod(super_cls, hs) : class_getInstanceMethod(super_cls, hs);
    IMP imp = m ? method_getImplementation(m) : class_getMethodImplementation(super_cls, hs);
    if (gsel == sel_dealloc && host == shadow_dealloc_in_progress) shadow_host_dealloc_done = true;
    call_host(r, host, super_cls, hs, imp, args_at, stret, class_method);
}

THUNK(objc_msgSendSuper) { msg_send_super(r, 0); }
THUNK(objc_msgSendSuper_stret) { msg_send_super(r, 1); }

// --- NSApp -------------------------------------------------------------------------------------
GDATA(NSApp, 4, NULL);
static void update_nsapp(void) {
    static id last;
    id app = NSApp;
    if (app == last) return;
    last = app;
    gwrite32(hostsym_lookup("_NSApp"), o2g(app));
}

// --- properties / sync ---------------------------------------------------------------------------
// objc_getProperty(id self, SEL _cmd, ptrdiff_t offset, BOOL atomic)
THUNK(objc_getProperty) {
    gaddr_t v = gread32(ARG(0) + ARG(2));
    id o = g2o(v);
    if (o && (ARG(3) & 0xff)) [[o retain] autorelease];
    RET(v);
}
// objc_setProperty(id self, SEL _cmd, ptrdiff_t offset, id newValue, BOOL atomic, signed char shouldCopy)
THUNK(objc_setProperty) {
    gaddr_t slot = ARG(0) + ARG(2);
    gaddr_t nv = ARG(3);
    id n = g2o(nv);
    if (n) {
        if (ARG(5) & 0xff) {
            id c = [n copy];
            nv = o2g(c);
        } else {
            [n retain];
        }
    }
    gaddr_t old = gread32(slot);
    gwrite32(slot, nv);
    if (old) [g2o(old) release];
}
THUNK(objc_sync_enter) { RET(objc_sync_enter(g2o(ARG(0)))); }
THUNK(objc_sync_exit) { RET(objc_sync_exit(g2o(ARG(0)))); }
THUNK(objc_enumerationMutation) { fatal("objc: collection mutated during fast enumeration"); }

// --- fast enumeration ----------------------------------------------------------------------------
// i386 NSFastEnumerationState { unsigned long state; id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; }
struct enum_state {
    NSFastEnumerationState hs;
    gaddr_t mutations;
};

static void fast_enumerate(struct guest_regs *r, id recv, gaddr_t args_at) {
    gaddr_t gstate = gread32(args_at), gbuf = gread32(args_at + 4);
    uint32_t count = gread32(args_at + 8);
    struct enum_state *es;
    if (gread32(gstate) == 0) {
        es = calloc(1, sizeof *es);
        es->mutations = gheap_calloc(1, 4);
        memcpy(g2h(gstate + 12), &es, sizeof es); // keep host state in extra[0..1]
        gwrite32(gstate, 1);
    } else {
        memcpy(&es, g2h(gstate + 12), sizeof es);
    }
    __unsafe_unretained id hbuf[16];
    NSUInteger n = [recv countByEnumeratingWithState:&es->hs objects:hbuf count:count < 16 ? count : 16];
    for (NSUInteger i = 0; i < n; i++) gwrite32(gbuf + 4 * (uint32_t)i, o2g(es->hs.itemsPtr[i]));
    gwrite32(gstate + 4, gbuf);
    gwrite32(es->mutations, es->hs.mutationsPtr ? (uint32_t)*es->hs.mutationsPtr : 0);
    gwrite32(gstate + 8, es->mutations);
    if (n == 0) {
        gheap_free(es->mutations);
        free(es);
        memset(g2h(gstate + 12), 0, sizeof es);
    }
    r->ret = (uint32_t)n;
}

// --- printf-style methods with %@ --------------------------------------------------------------

// Format a guest varargs list with Foundation semantics (%@ = description).
NSString *gformat_objc(NSString *fmt, struct gva va) {
    const char *f = fmt.UTF8String;
    size_t fl = strlen(f);
    char *nf = malloc(fl * 2 + 1);
    size_t o = 0;
    // New guest va list where %@ arguments are replaced by C strings.
    gaddr_t nva = gheap_calloc(1, 4 * 64 + 16);
    uint32_t nvo = 0;
    NSMutableArray *keep = [NSMutableArray array];
    for (const char *p = f; *p;) {
        if (*p != '%') {
            nf[o++] = *p++;
            continue;
        }
        nf[o++] = *p++;
        if (*p == '%') {
            nf[o++] = *p++;
            continue;
        }
        // flags, width, precision, length
        int lmod = 0;
        while (*p && strchr("-+ #0'*.0123456789", *p)) {
            if (*p == '*') {
                gwrite32(nva + nvo, gva_u32(&va));
                nvo += 4;
            }
            nf[o++] = *p++;
        }
        while (*p && strchr("hlqLjzt", *p)) {
            if (*p == 'l' && lmod == 3) lmod = 4;
            else if (*p == 'l') lmod = 3;
            else if (*p == 'q' || *p == 'j') lmod = 4;
            else if (*p == 'L') lmod = 6;
            nf[o++] = *p++;
        }
        char c = *p ? *p++ : 0;
        if (c == '@') {
            id obj = g2o(gva_u32(&va));
            NSString *d = obj ? [obj description] : @"(null)";
            [keep addObject:d];
            nf[o++] = 's';
            gwrite32(nva + nvo, gheap_strdup(d.UTF8String ?: ""));
            nvo += 4;
            continue;
        }
        nf[o++] = c;
        if (strchr("eEfFgGaA", c)) {
            uint32_t sz = lmod == 6 ? 16 : 8;
            memcpy(g2h(nva + nvo), g2h(va.p), sz);
            va.p += sz;
            nvo += sz;
        } else if (lmod == 4) {
            uint64_t v = gva_u64(&va);
            memcpy(g2h(nva + nvo), &v, 8);
            nvo += 8;
        } else if (c) {
            gwrite32(nva + nvo, gva_u32(&va));
            nvo += 4;
        }
        if (nvo > 4 * 60) break;
    }
    nf[o] = 0;
    char *out = NULL;
    gfmt_vasprintf(&out, nf, gva_at(nva));
    NSString *s = out ? [NSString stringWithUTF8String:out] : @"";
    free(out);
    free(nf);
    gheap_free(nva); // the %@ strings are leaked intentionally (small, rare)
    return s;
}

static bool special_format_selector(SEL sel) {
    return sel == @selector(stringWithFormat:) || sel == @selector(initWithFormat:) ||
           sel == @selector(appendFormat:) || sel == @selector(stringByAppendingFormat:) ||
           sel == @selector(localizedStringWithFormat:) || sel == @selector(raise:format:);
}

static void call_format_method(struct guest_regs *r, id recv, SEL sel, gaddr_t args_at, Class lookup) {
    if (sel == @selector(raise:format:)) {
        NSString *name = g2o(gread32(args_at));
        NSString *s = gformat_objc(g2o(gread32(args_at + 4)), gva_at(args_at + 8));
        NSException *e = [[NSException alloc] initWithName:name reason:s userInfo:nil];
        return guest_throw(r, o2g(e));
    }
    NSString *s = gformat_objc(g2o(gread32(args_at)), gva_at(args_at + 4));
    id res = nil;
    if (sel == @selector(stringWithFormat:) || sel == @selector(localizedStringWithFormat:))
        res = [(Class)recv stringWithString:s];
    else if (sel == @selector(initWithFormat:))
        res = [recv initWithString:s];
    else if (sel == @selector(appendFormat:))
        [recv appendString:s];
    else if (sel == @selector(stringByAppendingFormat:))
        res = [recv stringByAppendingString:s];
    (void)lookup;
    r->ret = o2g(res);
}

THUNK(NSLog) {
    NSString *s = gformat_objc(g2o(ARG(0)), GVA_FROM_ARG(1));
    NSLog(@"[guest] %@", s);
}
THUNK(NSLogv) {
    NSString *s = gformat_objc(g2o(ARG(0)), gva_at(ARG(1)));
    NSLog(@"[guest] %@", s);
}

// --- guest blocks passed to host APIs -------------------------------------------------------------
// Parameter types come from the block's clang signature (BLOCK_HAS_SIGNATURE);
// without one, up to three object arguments are assumed (completion handlers).
// Integer/pointer arguments arrive in the same host registers as objects.
static int guest_block_params(gaddr_t b, char kinds[3]) {
    uint32_t flags = gread32(b + 4);
    gaddr_t desc = gread32(b + 16);
    if (!(flags & (1u << 30)) || !desc) {
        memset(kinds, '@', 3);
        return 3;
    }
    gaddr_t sig_at = desc + 8 + ((flags & (1u << 25)) ? 8 : 0);
    const char *sig = g2h(gread32(sig_at));
    // "v12@?0@4c8": return type, then the block itself, then the parameters
    int n = -2;
    for (const char *t = sig; *t && n < 3;) {
        while (*t && strchr("rnNoORV", *t)) t++;
        char k = *t;
        if (n >= 0) kinds[n] = k;
        if (k == '@' && t[1] == '?') t++;
        if (k == '@' && t[1] == '"') for (t += 2; *t && *t != '"'; t++) {}
        if (k == '^') while (*t == '^') t++;
        if (k == '{' || k == '(') {
            int depth = 0;
            do {
                if (*t == '{' || *t == '(') depth++;
                else if (*t == '}' || *t == ')') depth--;
                t++;
            } while (*t && depth);
            t--;
        }
        if (*t) t++;
        while (*t >= '0' && *t <= '9') t++;
        n++;
    }
    return n < 0 ? 0 : n;
}

id guest_block_to_host(gaddr_t g) {
    gaddr_t b = gblock_copy(g);
    char kinds[3];
    int n = guest_block_params(b, kinds);
    uint32_t objs = 0; // bit i: parameter i is an object
    for (int i = 0; i < n; i++)
        if (kinds[i] == '@') objs |= 1u << i;
    void (^hb)(uintptr_t, uintptr_t, uintptr_t) = ^(uintptr_t a0, uintptr_t a1, uintptr_t a2) {
        cpu_thread_attach();
        uintptr_t in[3] = {a0, a1, a2};
        uint32_t args[4] = {b};
        for (int i = 0; i < n; i++)
            args[i + 1] = (objs >> i) & 1 ? o2g((__bridge id)(void *)in[i]) : (uint32_t)in[i];
        cpu_call(gread32(b + 12), 1 + n, args);
    };
    return [[hb copy] autorelease];
}
