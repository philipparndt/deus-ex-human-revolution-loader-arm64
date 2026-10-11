// Carbon Event Manager emulation: EventRef objects with typed parameters, event
// targets and handler chains, the main event queue/loop (fed by AppKit NSEvents),
// event loop timers, keyboard/mouse state.
//
// Dispatch model (SendEventToEventTarget): the chain for a target is
//   target handlers (newest first) -> host default for that target
//   -> parent target handlers -> parent default -> ... -> application
// where parents are: view -> superview ... -> window -> application,
// menu -> application. The dispatcher target's default routes the event to the
// user focus (keyboard), the window under the mouse (mouse) or the application.
// The application default forwards events that came from AppKit and were not
// handled by the guest to [NSApp sendEvent:] so AppKit UI keeps working.
#import "carbon_internal.h"
#include "mainq.h"
#include "cpu.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>

void cb_window_module_init(void);
void cb_menu_module_init(void);

// --- helpers -------------------------------------------------------------------------------------

const char *cb_fourcc(uint32_t v, char buf[8]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)(v >> (24 - 8 * i));
        buf[i] = (c >= 32 && c < 127) ? c : '.';
    }
    buf[4] = 0;
    return buf;
}

@implementation CBBase
- (void)dealloc {
    if (handle) hobj_forget(handle);
    [super dealloc];
}
@end

gaddr_t cb_handle(id o) {
    if (!o) return 0;
    if ([o isKindOfClass:[CBBase class]]) {
        CBBase *b = o;
        if (!b->handle) b->handle = hobj_wrap(o);
        return b->handle;
    }
    return hobj_wrap(o);
}

id cb_unwrap(gaddr_t g, Class cls) {
    if (!g || !hobj_is_handle(g)) return nil;
    id o = (id)hobj_unwrap(g);
    if (!o) return nil;
    // Only our own objects are ObjC objects we know; check the class defensively.
    if (cls && ![o isKindOfClass:cls]) return nil;
    return o;
}

void cb_on_main(void (^blk)(void)) {
    if ([NSThread isMainThread]) blk();
    else {
        extern void cgl_drop_held_locks(void), cgl_retake_held_locks(void);
        cgl_drop_held_locks(); // see objc_msg.m
        mainq_run(blk);
        cgl_retake_held_locks();
    }
}

double cb_now(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

double cb_main_screen_height(void) { return CGDisplayBounds(CGMainDisplayID()).size.height; }
NSPoint cb_global_to_ns(CGPoint p) { return NSMakePoint(p.x, cb_main_screen_height() - p.y); }
CGPoint cb_ns_to_global(NSPoint p) { return CGPointMake(p.x, cb_main_screen_height() - p.y); }
NSRect cb_global_rect_to_ns(CGRect rc) {
    return NSMakeRect(rc.origin.x, cb_main_screen_height() - rc.origin.y - rc.size.height, rc.size.width,
                      rc.size.height);
}
CGRect cb_ns_rect_to_global(NSRect rc) {
    return CGRectMake(rc.origin.x, cb_main_screen_height() - rc.origin.y - rc.size.height, rc.size.width,
                      rc.size.height);
}

// --- application setup -----------------------------------------------------------------------------

static void app_notifications(void);

// The game bundle's icon (the host process is a bare executable).
NSImage *cb_game_icon(void) {
    static NSImage *icon;
    if (!icon) {
        NSBundle *b = [NSBundle mainBundle]; // swizzled to the game .app
        NSString *name = [b objectForInfoDictionaryKey:@"CFBundleIconFile"];
        NSString *path = name ? [b pathForResource:[name stringByDeletingPathExtension] ofType:@"icns"] : nil;
        if (path) icon = [[NSImage alloc] initWithContentsOfFile:path];
        if (!icon && b.bundlePath) icon = [[[NSWorkspace sharedWorkspace] iconForFile:b.bundlePath] retain];
    }
    return icon;
}

void cb_app_init(void) {
    static bool done;
    if (done) return;
    if (![NSThread isMainThread]) return; // only from the main thread
    done = true;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (cb_game_icon()) [NSApp setApplicationIconImage:cb_game_icon()];
        cb_menu_module_init();
        if (![[NSRunningApplication currentApplication] isFinishedLaunching]) [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        cb_window_module_init();
        app_notifications();
        gaddr_t nsapp = hostsym_lookup("_NSApp");
        if (nsapp) gwrite32(nsapp, o2g(NSApp));
    }
    LOGI("carbon: application initialised");
}

// --- targets & handlers ---------------------------------------------------------------------------

@implementation CBTarget
- (instancetype)initWithKind:(int)k owner:(id)o {
    if ((self = [super init])) {
        kind = k;
        owner = o;
        handlers = [[NSMutableArray alloc] init];
    }
    return self;
}
- (void)dealloc {
    for (CBHandler *h in handlers) {
        h->target = nil;
        h->removed = true;
    }
    [handlers release];
    [super dealloc];
}
@end

@implementation CBHandler
- (void)dealloc {
    free(types);
    [super dealloc];
}
@end

static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;

static CBTarget *app_target, *disp_target;
CBTarget *cb_app_target(void) {
    if (!app_target) app_target = [[CBTarget alloc] initWithKind:TK_APP owner:nil];
    return app_target;
}
CBTarget *cb_dispatcher_target(void) {
    if (!disp_target) disp_target = [[CBTarget alloc] initWithKind:TK_DISPATCHER owner:nil];
    return disp_target;
}

CBTarget *cb_target_parent(CBTarget *t) {
    switch (t->kind) {
    case TK_VIEW: {
        CBView *v = t->owner;
        if (!v) return nil;
        if (v->superview) return v->superview->target;
        if (v->window) return v->window->target;
        return nil;
    }
    case TK_WINDOW: return cb_app_target();
    case TK_MENU: return cb_app_target();
    default: return nil;
    }
}

static bool handler_matches(CBHandler *h, uint32_t cls, uint32_t kind) {
    for (uint32_t i = 0; i < h->ntypes; i++)
        if (h->types[2 * i] == cls && h->types[2 * i + 1] == kind) return true;
    return false;
}

static void handler_add_types(CBHandler *h, uint32_t n, const uint32_t *t) {
    pthread_mutex_lock(&handler_lock);
    h->types = realloc(h->types, (h->ntypes + n) * 8);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = t[2 * i], k = t[2 * i + 1];
        bool dup = false;
        for (uint32_t j = 0; j < h->ntypes; j++)
            if (h->types[2 * j] == c && h->types[2 * j + 1] == k) dup = true;
        if (dup) continue;
        h->types[2 * h->ntypes] = c;
        h->types[2 * h->ntypes + 1] = k;
        h->ntypes++;
    }
    pthread_mutex_unlock(&handler_lock);
}

CBHandler *cb_install_handler(CBTarget *t, gaddr_t upp, uint32_t ntypes, const uint32_t *types, gaddr_t udata) {
    CBHandler *h = [[CBHandler alloc] init];
    h->target = t;
    h->upp = upp;
    h->udata = udata;
    handler_add_types(h, ntypes, types);
    pthread_mutex_lock(&handler_lock);
    [t->handlers insertObject:h atIndex:0];
    pthread_mutex_unlock(&handler_lock);
    [h release];
    return h;
}

// --- events ------------------------------------------------------------------------------------------

@implementation CBParam
- (void)dealloc {
    [data release];
    if (cf) CFRelease(cf);
    [super dealloc];
}
@end

static bool type_is_cf(uint32_t t) {
    switch (t) {
    case typeCFStringRef: case typeCFMutableStringRef: case typeCFTypeRef: case typeCFArrayRef:
    case typeCFMutableArrayRef: case typeCFDictionaryRef: case typeCFMutableDictionaryRef:
    case typeCFNumberRef: case typeCFBooleanRef: case typeCFAttributedStringRef:
    case typeCFMutableAttributedStringRef: case typeEventRef:
        return true;
    default: return false;
    }
}

// Events are pooled: a CBEvent whose retain count drops to zero is cleared and
// reused, so its guest handle stays valid and handle slots are not exhausted by
// the stream of input events.
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static CBEvent **event_pool; // plain pointers, no retain/release involved
static size_t pool_n, pool_cap;

@implementation CBEvent {
    _Atomic int rc;
}
- (instancetype)retain {
    atomic_fetch_add(&rc, 1);
    return self;
}
- (oneway void)release {
    if (atomic_fetch_sub(&rc, 1) != 1) return;
    @synchronized(params) {
        [params removeAllObjects];
    }
    [ns release];
    ns = nil;
    pthread_mutex_lock(&pool_lock);
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 64;
        event_pool = realloc(event_pool, sizeof(CBEvent *) * pool_cap);
    }
    event_pool[pool_n++] = self;
    pthread_mutex_unlock(&pool_lock);
}
- (NSUInteger)retainCount { return (NSUInteger)atomic_load(&rc); }
@end

CBEvent *cb_event_new(uint32_t cls, uint32_t kind, double when) {
    CBEvent *e = nil;
    pthread_mutex_lock(&pool_lock);
    if (pool_n) e = event_pool[--pool_n];
    pthread_mutex_unlock(&pool_lock);
    if (!e) {
        // alloc's implicit reference is never dropped: CBEvents are never deallocated.
        e = [[CBEvent alloc] init];
        e->params = [[NSMutableDictionary alloc] init];
    }
    [e retain]; // our count: 0 -> 1
    e->cls = cls;
    e->kind = kind;
    e->time = when;
    e->attrs = 0;
    e->ns = nil;
    e->ns_forwarded = false;
    e->queued = false;
    e->priority = kEventPriorityStandard;
    return e;
}

void cb_event_set(CBEvent *e, uint32_t name, uint32_t type, const void *data, uint32_t size) {
    CBParam *p = [[CBParam alloc] init];
    p->type = type;
    p->data = [[NSData alloc] initWithBytes:data length:size];
    if (type_is_cf(type) && size >= 4) {
        uint32_t g;
        memcpy(&g, data, 4);
        if (type == typeEventRef) {
            CBEvent *ev = CB_UNWRAP(CBEvent, g);
            if (ev) p->cf = CFRetain(ev);
        } else if (g) {
            CFTypeRef o = gcf(g);
            if (o) p->cf = CFRetain(o);
        }
    }
    @synchronized(e->params) {
        [e->params setObject:p forKey:@(name)];
    }
    [p release];
}

CBParam *cb_event_param(CBEvent *e, uint32_t name) {
    @synchronized(e->params) {
        return [[[e->params objectForKey:@(name)] retain] autorelease];
    }
}

bool cb_event_get_u32(CBEvent *e, uint32_t name, uint32_t *out) {
    CBParam *p = cb_event_param(e, name);
    if (!p || p->data.length < 4) return false;
    memcpy(out, p->data.bytes, 4);
    return true;
}

#define EV(i) CB_UNWRAP(CBEvent, ARG(i))

// CreateEvent(CFAllocatorRef, UInt32 class, UInt32 kind, EventTime when, EventAttributes, EventRef *out)
THUNK(CreateEvent) {
    double when = ARGD(3);
    if (when == 0) when = cb_now();
    CBEvent *e = cb_event_new(ARG(1), ARG(2), when);
    e->attrs = ARG(5);
    if (ARG(6)) gwrite32(ARG(6), cb_handle(e));
    RET(noErr);
}
THUNK(RetainEvent) {
    CBEvent *e = EV(0);
    [e retain];
    RET(ARG(0));
}
THUNK(ReleaseEvent) {
    CBEvent *e = EV(0);
    [e release];
}
THUNK(GetEventClass) {
    CBEvent *e = EV(0);
    RET(e ? e->cls : 0);
}
THUNK(GetEventKind) {
    CBEvent *e = EV(0);
    RET(e ? e->kind : 0);
}
THUNK(GetEventTime) {
    CBEvent *e = EV(0);
    RETD(e ? e->time : 0.0);
}
THUNK(GetCurrentEventTime) { RETD(cb_now()); }

static bool is_int_type(uint32_t t, uint32_t *size, bool *sgn) {
    switch (t) {
    case typeSInt16: *size = 2; *sgn = true; return true;
    case typeUInt16: *size = 2; *sgn = false; return true;
    case typeSInt32: *size = 4; *sgn = true; return true;
    case typeUInt32: *size = 4; *sgn = false; return true;
    case typeSInt64: *size = 8; *sgn = true; return true;
    case typeUInt64: *size = 8; *sgn = false; return true;
    case typeMouseButton: case typeMouseWheelAxis: case typeWindowPartCode: case typeControlPartCode:
        *size = 2; *sgn = false; return true;
    case typeBoolean: *size = 1; *sgn = false; return true;
    default: return false;
    }
}

// Coerce parameter data to the desired type. Returns false if impossible.
static bool coerce(CBParam *p, uint32_t want, NSMutableData *out) {
    const uint8_t *d = p->data.bytes;
    uint32_t n = (uint32_t)p->data.length;
    if (want == typeWildCard || want == p->type) {
        [out appendBytes:d length:n];
        return true;
    }
    uint32_t sa, sb;
    bool sga, sgb;
    if (is_int_type(p->type, &sa, &sga) && is_int_type(want, &sb, &sgb) && n >= sa) {
        int64_t v = 0;
        if (sa == 1) v = d[0];
        else if (sa == 2) v = sga ? (int16_t)(d[0] | d[1] << 8) : (uint16_t)(d[0] | d[1] << 8);
        else if (sa == 4) { int32_t x; memcpy(&x, d, 4); v = sga ? x : (uint32_t)x; }
        else memcpy(&v, d, 8);
        [out appendBytes:&v length:sb];
        return true;
    }
    if (p->type == typeHIPoint && want == typeQDPoint && n >= 8) {
        float f[2];
        memcpy(f, d, 8);
        int16_t pt[2] = {(int16_t)lroundf(f[1]), (int16_t)lroundf(f[0])};
        [out appendBytes:pt length:4];
        return true;
    }
    if (p->type == typeQDPoint && want == typeHIPoint && n >= 4) {
        int16_t pt[2];
        memcpy(pt, d, 4);
        float f[2] = {pt[1], pt[0]};
        [out appendBytes:f length:8];
        return true;
    }
    if (p->type == typeHIRect && want == typeQDRectangle && n >= 16) {
        float f[4];
        memcpy(f, d, 16);
        int16_t q[4] = {(int16_t)lroundf(f[1]), (int16_t)lroundf(f[0]), (int16_t)lroundf(f[1] + f[3]),
                        (int16_t)lroundf(f[0] + f[2])};
        [out appendBytes:q length:8];
        return true;
    }
    if (p->type == typeQDRectangle && want == typeHIRect && n >= 8) {
        int16_t q[4];
        memcpy(q, d, 8);
        float f[4] = {q[1], q[0], q[3] - q[1], q[2] - q[0]};
        [out appendBytes:f length:16];
        return true;
    }
    if (p->type == typeHISize && want == typeHIPoint) {
        [out appendBytes:d length:n];
        return true;
    }
    if (p->type == typeUnicodeText && want == typeChar) {
        CFStringRef s = CFStringCreateWithCharacters(NULL, (const UniChar *)d, n / 2);
        char buf[256];
        CFIndex used = 0;
        CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingMacRoman, '?', false,
                         (UInt8 *)buf, sizeof buf, &used);
        CFRelease(s);
        [out appendBytes:buf length:(NSUInteger)used];
        return true;
    }
    if (p->type == typeChar && want == typeUnicodeText) {
        CFStringRef s = CFStringCreateWithBytes(NULL, d, n, kCFStringEncodingMacRoman, false);
        CFIndex len = s ? CFStringGetLength(s) : 0;
        UniChar *u = calloc((size_t)len + 1, 2);
        if (s) CFStringGetCharacters(s, CFRangeMake(0, len), u);
        [out appendBytes:u length:(NSUInteger)len * 2];
        free(u);
        if (s) CFRelease(s);
        return true;
    }
    if ((p->type == typeUnicodeText || p->type == typeChar) && want == typeCFStringRef) {
        CFStringRef s = p->type == typeUnicodeText
                            ? CFStringCreateWithCharacters(NULL, (const UniChar *)d, n / 2)
                            : CFStringCreateWithBytes(NULL, d, n, kCFStringEncodingMacRoman, false);
        uint32_t g = cfg(s); // owned by the caller (Carbon returns a +1 string here)
        [out appendBytes:&g length:4];
        return true;
    }
    if (type_is_cf(p->type) && type_is_cf(want) && p->type != typeEventRef && want != typeEventRef) {
        [out appendBytes:d length:n];
        return true;
    }
    // Object references: allow WindowRef/ControlRef/HIObjectRef/... interchangeably.
    switch (p->type) {
    case typeWindowRef: case typeControlRef: case typeHIObjectRef: case typeMenuRef: case typeVoidPtr:
    case typeEventTargetRef:
        switch (want) {
        case typeWindowRef: case typeControlRef: case typeHIObjectRef: case typeMenuRef: case typeVoidPtr:
        case typeEventTargetRef:
            [out appendBytes:d length:n];
            return true;
        }
    }
    if (p->type == typeHICommand && want == typeHICommand) {
        [out appendBytes:d length:n];
        return true;
    }
    return false;
}

// GetEventParameter(event, name, desiredType, *outActualType, bufferSize, *outActualSize, outData)
THUNK(GetEventParameter) {
    CBEvent *e = EV(0);
    if (!e) RET(paramErr);
    uint32_t name = ARG(1), want = ARG(2);
    CBParam *p = cb_event_param(e, name);
    char b1[8], b2[8];
    if (!p) {
        LOGT("carbon: GetEventParameter(%s) not found", cb_fourcc(name, b1));
        RET(eventParameterNotFoundErr);
    }
    NSMutableData *d = [NSMutableData data];
    if (!coerce(p, want, d)) {
        LOGD("carbon: GetEventParameter(%s): cannot coerce %s to %s", cb_fourcc(name, b1),
             cb_fourcc(p->type, b2), cb_fourcc(want, (char[8]){0}));
        RET(errAECoercionFail);
    }
    if (ARG(3)) gwrite32(ARG(3), want == typeWildCard ? p->type : want);
    if (ARG(5)) gwrite32(ARG(5), (uint32_t)d.length);
    if (ARG(6)) {
        uint32_t n = (uint32_t)d.length;
        if (n > ARG(4)) n = ARG(4);
        memcpy(g2h(ARG(6)), d.bytes, n);
    }
    RET(noErr);
}

THUNK(SetEventParameter) {
    CBEvent *e = EV(0);
    if (!e) RET(paramErr);
    cb_event_set(e, ARG(1), ARG(2), ARG(4) ? g2h(ARG(4)) : "", ARG(4) ? ARG(3) : 0);
    RET(noErr);
}

THUNK(RemoveEventParameter) {
    CBEvent *e = EV(0);
    if (!e) RET(paramErr);
    bool had;
    @synchronized(e->params) {
        had = [e->params objectForKey:@(ARG(1))] != nil;
        [e->params removeObjectForKey:@(ARG(1))];
    }
    RET(had ? noErr : eventParameterNotFoundErr);
}

// --- dispatch ----------------------------------------------------------------------------------------

struct centry {
    CBHandler *h;
    cb_default_fn fn;
    CBTarget *t;
};

@interface CBCallRef : CBBase {
  @public
    CBEvent *ev;
    struct centry *e;
    int n, cap, pos;
}
@end
@implementation CBCallRef
@end

static NSMutableArray *callref_pool;
static pthread_mutex_t callref_lock = PTHREAD_MUTEX_INITIALIZER;

static CBCallRef *callref_get(void) {
    CBCallRef *c = nil;
    pthread_mutex_lock(&callref_lock);
    if (callref_pool.count) {
        c = [[callref_pool lastObject] retain];
        [callref_pool removeLastObject];
    }
    pthread_mutex_unlock(&callref_lock);
    if (!c) c = [[CBCallRef alloc] init];
    c->n = c->pos = 0;
    return c;
}
static void callref_put(CBCallRef *c) {
    for (int i = 0; i < c->n; i++) [c->e[i].h release];
    c->n = 0;
    [c->ev release];
    c->ev = nil;
    pthread_mutex_lock(&callref_lock);
    if (!callref_pool) callref_pool = [[NSMutableArray alloc] init];
    [callref_pool addObject:c];
    pthread_mutex_unlock(&callref_lock);
    [c release];
}

static void chain_add(CBCallRef *c, CBHandler *h, cb_default_fn fn, CBTarget *t) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->e = realloc(c->e, sizeof(struct centry) * (size_t)c->cap);
    }
    c->e[c->n++] = (struct centry){[h retain], fn, t};
}

static OSStatus dispatcher_default(CBEvent *ev, CBTarget *t);
static OSStatus app_default(CBEvent *ev, CBTarget *t);
OSStatus cb_window_default(CBEvent *ev, CBTarget *t);
OSStatus cb_view_default(CBEvent *ev, CBTarget *t);

static cb_default_fn default_for(CBTarget *t) {
    switch (t->kind) {
    case TK_DISPATCHER: return dispatcher_default;
    case TK_APP: return app_default;
    case TK_WINDOW: return cb_window_default;
    case TK_VIEW: return cb_view_default;
    default: return NULL;
    }
}

static OSStatus call_entry(CBCallRef *c, int i) {
    struct centry *en = &c->e[i];
    if (en->h) {
        CBHandler *h = en->h;
        if (h->removed) return eventNotHandledErr;
        char b[8];
        LOGT("carbon: -> handler %#x for %s/%u", h->upp, cb_fourcc(c->ev->cls, b), c->ev->kind);
        OSStatus st = (OSStatus)gcall3(h->upp, cb_handle(c), cb_handle(c->ev), h->udata);
        LOGT("carbon: <- handler %#x = %d", h->upp, (int)st);
        return st;
    }
    return en->fn(c->ev, en->t);
}

static OSStatus run_chain(CBCallRef *c, int start) {
    for (c->pos = start; c->pos < c->n; c->pos++) {
        OSStatus st = call_entry(c, c->pos);
        if (st != eventNotHandledErr) {
            c->pos = c->n;
            return st;
        }
    }
    return eventNotHandledErr;
}

static OSStatus send_ex(CBEvent *ev, CBTarget *t, bool propagate) {
    if (!ev || !t) return paramErr;
    CBCallRef *c = callref_get();
    c->ev = [ev retain];
    pthread_mutex_lock(&handler_lock);
    for (CBTarget *x = t; x; x = propagate ? cb_target_parent(x) : nil) {
        for (CBHandler *h in x->handlers)
            if (!h->removed && handler_matches(h, ev->cls, ev->kind)) chain_add(c, h, NULL, x);
        cb_default_fn fn = default_for(x);
        if (fn) chain_add(c, nil, fn, x);
    }
    pthread_mutex_unlock(&handler_lock);
    OSStatus st = run_chain(c, 0);
    callref_put(c);
    return st;
}

OSStatus cb_send(CBEvent *ev, CBTarget *t) { return send_ex(ev, t, true); }
// kEventTargetDontPropagate: only the target's own handlers (HIView notifications).
OSStatus cb_send_noprop(CBEvent *ev, CBTarget *t) { return send_ex(ev, t, false); }

THUNK(CallNextEventHandler) {
    CBCallRef *c = CB_UNWRAP(CBCallRef, ARG(0));
    if (!c) RET(eventNotHandledErr);
    RET(run_chain(c, c->pos + 1));
}

THUNK(SendEventToEventTarget) {
    CBEvent *e = EV(0);
    CBTarget *t = CB_UNWRAP(CBTarget, ARG(1));
    char b[8];
    if (e) LOGT("carbon: SendEventToEventTarget(%s/%u, kind %d)", cb_fourcc(e->cls, b), e->kind, t ? t->kind : -1);
    RET(cb_send(e, t));
}

THUNK(GetApplicationEventTarget) { RET(cb_handle(cb_app_target())); }
THUNK(GetEventDispatcherTarget) { RET(cb_handle(cb_dispatcher_target())); }

// InstallEventHandler(target, upp, numTypes, list, userData, outRef)
THUNK(InstallEventHandler) {
    CBTarget *t = CB_UNWRAP(CBTarget, ARG(0));
    if (!t) {
        LOGE("carbon: InstallEventHandler on unknown target %#x", ARG(0));
        RET(paramErr);
    }
    if ([NSThread isMainThread]) cb_app_init();
    uint32_t n = ARG(2);
    CBHandler *h = cb_install_handler(t, ARG(1), n, n ? g2h(ARG(3)) : NULL, ARG(4));
    if (log_level >= 2) {
        for (uint32_t i = 0; i < n; i++) {
            char b[8];
            LOGD("carbon: InstallEventHandler(target kind %d, upp %#x): %s/%u", t->kind, ARG(1),
                 cb_fourcc(gread32(ARG(3) + 8 * i), b), gread32(ARG(3) + 8 * i + 4));
        }
    }
    if (ARG(5)) gwrite32(ARG(5), cb_handle(h));
    RET(noErr);
}

THUNK(RemoveEventHandler) {
    CBHandler *h = CB_UNWRAP(CBHandler, ARG(0));
    if (!h) RET(paramErr);
    pthread_mutex_lock(&handler_lock);
    h->removed = true;
    CBTarget *t = h->target;
    h->target = nil;
    [h retain];
    if (t) [t->handlers removeObjectIdenticalTo:h];
    pthread_mutex_unlock(&handler_lock);
    [h release];
    RET(noErr);
}

THUNK(AddEventTypesToHandler) {
    CBHandler *h = CB_UNWRAP(CBHandler, ARG(0));
    if (!h) RET(paramErr);
    handler_add_types(h, ARG(1), g2h(ARG(2)));
    RET(noErr);
}

THUNK(RemoveEventTypesFromHandler) {
    CBHandler *h = CB_UNWRAP(CBHandler, ARG(0));
    if (!h) RET(paramErr);
    pthread_mutex_lock(&handler_lock);
    for (uint32_t i = 0; i < ARG(1); i++) {
        uint32_t c = gread32(ARG(2) + 8 * i), k = gread32(ARG(2) + 8 * i + 4);
        for (uint32_t j = 0; j < h->ntypes; j++) {
            if (h->types[2 * j] == c && h->types[2 * j + 1] == k) {
                memmove(&h->types[2 * j], &h->types[2 * j + 2], (h->ntypes - j - 1) * 8);
                h->ntypes--;
                break;
            }
        }
    }
    pthread_mutex_unlock(&handler_lock);
    RET(noErr);
}

THUNK(InstallStandardEventHandler) {
    CBTarget *t = CB_UNWRAP(CBTarget, ARG(0));
    if (!t) RET(paramErr);
    t->standard = true;
    RET(noErr);
}

// --- keyboard state ----------------------------------------------------------------------------------

static uint8_t keymap[16];
static uint32_t last_event_mods;

uint32_t cb_modifiers_from_ns(NSEventModifierFlags f) {
    uint32_t m = 0;
    if (f & NSEventModifierFlagCommand) m |= cmdKey;
    if (f & NSEventModifierFlagShift) m |= shiftKey;
    if (f & NSEventModifierFlagCapsLock) m |= alphaLock;
    if (f & NSEventModifierFlagOption) m |= optionKey;
    if (f & NSEventModifierFlagControl) m |= controlKey;
    if (f & 0x4) m |= rightShiftKey;    // NX_DEVICERSHIFTKEYMASK
    if (f & 0x40) m |= rightOptionKey;  // NX_DEVICERALTKEYMASK
    if (f & 0x2000) m |= rightControlKey; // NX_DEVICERCTLKEYMASK
    if (f & NSEventModifierFlagNumericPad) m |= kEventKeyModifierNumLockMask;
    if (f & NSEventModifierFlagFunction) m |= kEventKeyModifierFnMask;
    return m;
}

uint32_t cb_current_modifiers(void) {
    uint32_t m = cb_modifiers_from_ns([NSEvent modifierFlags]);
    if ([NSEvent pressedMouseButtons] & 1) m |= btnState;
    return m;
}

static void keymap_set(unsigned code, bool down) {
    if (code >= 128) return;
    if (down) keymap[code >> 3] |= (uint8_t)(1u << (code & 7));
    else keymap[code >> 3] &= (uint8_t)~(1u << (code & 7));
}

static void keymap_update_modifiers(NSEventModifierFlags f) {
    keymap_set(kVK_Command, f & 0x8);
    keymap_set(kVK_RightCommand, f & 0x10);
    keymap_set(kVK_Shift, f & 0x2);
    keymap_set(kVK_RightShift, f & 0x4);
    keymap_set(kVK_Option, f & 0x20);
    keymap_set(kVK_RightOption, f & 0x40);
    keymap_set(kVK_Control, f & 0x1);
    keymap_set(kVK_RightControl, f & 0x2000);
    keymap_set(kVK_CapsLock, f & NSEventModifierFlagCapsLock);
    keymap_set(kVK_Function, f & NSEventModifierFlagFunction);
}

void cb_keymap_clear(void) { memset(keymap, 0, sizeof keymap); }

THUNK(GetKeys) {
    if (!ARG(0)) return;
    keymap_update_modifiers([NSEvent modifierFlags]);
    memcpy(g2h(ARG(0)), keymap, 16);
}
THUNK(GetCurrentKeyModifiers) { RET(cb_current_modifiers() & ~(uint32_t)btnState); }
THUNK(GetCurrentEventKeyModifiers) { RET(last_event_mods); }
THUNK(GetDblTime) { RET((uint32_t)lround([NSEvent doubleClickInterval] * 60.0)); }
THUNK(GetCaretTime) { RET(32); }
THUNK(LMGetKbdType) { RET(LMGetKbdType()); }

// --- NSEvent -> Carbon event translation ---------------------------------------------------------------

static uint8_t mac_char_for(NSEvent *e, unsigned short code, UniChar *uni) {
    switch (code) {
    case kVK_LeftArrow: *uni = 0x1C; return 0x1C;
    case kVK_RightArrow: *uni = 0x1D; return 0x1D;
    case kVK_UpArrow: *uni = 0x1E; return 0x1E;
    case kVK_DownArrow: *uni = 0x1F; return 0x1F;
    case kVK_Delete: *uni = 0x08; return 0x08;
    case kVK_ForwardDelete: *uni = 0x7F; return 0x7F;
    case kVK_Home: *uni = 0x01; return 0x01;
    case kVK_End: *uni = 0x04; return 0x04;
    case kVK_PageUp: *uni = 0x0B; return 0x0B;
    case kVK_PageDown: *uni = 0x0C; return 0x0C;
    case kVK_Escape: *uni = 0x1B; return 0x1B;
    case kVK_Return: *uni = 0x0D; return 0x0D;
    case kVK_ANSI_KeypadEnter: *uni = 0x03; return 0x03;
    case kVK_Tab: *uni = 0x09; return 0x09;
    case kVK_Help: *uni = 0x05; return 0x05;
    case kVK_F1: case kVK_F2: case kVK_F3: case kVK_F4: case kVK_F5: case kVK_F6: case kVK_F7: case kVK_F8:
    case kVK_F9: case kVK_F10: case kVK_F11: case kVK_F12: case kVK_F13: case kVK_F14: case kVK_F15:
    case kVK_F16: case kVK_F17: case kVK_F18: case kVK_F19: case kVK_F20:
        *uni = 0x10;
        return 0x10;
    }
    NSString *s = nil;
    @try {
        s = [e characters];
    } @catch (id x) {
        s = nil;
    }
    if (!s.length) {
        *uni = 0;
        return 0;
    }
    *uni = [s characterAtIndex:0];
    if (*uni >= 0xF700 && *uni <= 0xF8FF) {
        *uni = 0;
        return 0;
    }
    UInt8 c = 0;
    CFIndex used = 0;
    CFStringGetBytes((CFStringRef)s, CFRangeMake(0, 1), kCFStringEncodingMacRoman, '?', false, &c, 1, &used);
    return used ? c : '?';
}

static void set_key_params(CBEvent *c, NSEvent *e) {
    unsigned short code = [e keyCode];
    UniChar uni = 0;
    uint8_t ch = mac_char_for(e, code, &uni);
    cb_event_set(c, kEventParamKeyMacCharCodes, typeChar, &ch, 1);
    cb_event_set_u32(c, kEventParamKeyCode, typeUInt32, code);
    cb_event_set_u32(c, kEventParamKeyModifiers, typeUInt32, cb_modifiers_from_ns([e modifierFlags]));
    cb_event_set_u32(c, kEventParamKeyboardType, typeUInt32, LMGetKbdType());
    NSString *s = nil;
    @try {
        s = [e characters];
    } @catch (id x) {
        s = nil;
    }
    if (uni && (uni < 0x20 || uni == 0x7F)) {
        cb_event_set(c, kEventParamKeyUnicodes, typeUnicodeText, &uni, 2);
    } else if (s.length && !([s characterAtIndex:0] >= 0xF700 && [s characterAtIndex:0] <= 0xF8FF)) {
        NSUInteger n = s.length;
        UniChar *buf = malloc(n * 2);
        [s getCharacters:buf range:NSMakeRange(0, n)];
        cb_event_set(c, kEventParamKeyUnicodes, typeUnicodeText, buf, (uint32_t)n * 2);
        free(buf);
    } else {
        cb_event_set(c, kEventParamKeyUnicodes, typeUnicodeText, "", 0);
    }
}

static CGPoint ns_event_global(NSEvent *e) {
    NSPoint p = [e locationInWindow];
    if ([e window]) p = [[e window] convertPointToScreen:p];
    return cb_ns_to_global(p);
}

static void set_mouse_params(CBEvent *c, NSEvent *e) {
    CGPoint g = ns_event_global(e);
    cb_event_set_hipoint(c, kEventParamMouseLocation, g);
    CBWindow *w = cb_window_for_ns([e window]);
    if (!w) {
        // window under the mouse
        NSInteger num = [NSWindow windowNumberAtPoint:cb_global_to_ns(g) belowWindowWithWindowNumber:0];
        w = cb_window_for_ns([NSApp windowWithWindowNumber:num]);
    }
    if (w) {
        CGRect s = cb_window_structure_global(w);
        cb_event_set_hipoint(c, kEventParamWindowMouseLocation, CGPointMake(g.x - s.origin.x, g.y - s.origin.y));
        cb_event_set_u32(c, kEventParamWindowRef, typeWindowRef, cb_handle(w));
        CGRect ct = cb_window_content_global(w);
        uint16_t part = CGRectContainsPoint(ct, g) ? inContent : inDrag;
        cb_event_set_u16(c, kEventParamWindowPartCode, typeWindowPartCode, part);
    }
    cb_event_set_u32(c, kEventParamKeyModifiers, typeUInt32, cb_modifiers_from_ns([e modifierFlags]));
    float dx = 0, dy = 0;
    switch ([e type]) {
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
        dx = (float)[e deltaX];
        dy = (float)[e deltaY];
        break;
    default: break;
    }
    cb_event_set_hipoint(c, kEventParamMouseDelta, CGPointMake(dx, dy));
    uint16_t button = (uint16_t)([e type] == NSEventTypeScrollWheel ? 1 : [e buttonNumber] + 1);
    if ([e type] == NSEventTypeLeftMouseDown || [e type] == NSEventTypeLeftMouseUp ||
        [e type] == NSEventTypeLeftMouseDragged)
        button = 1;
    cb_event_set_u16(c, kEventParamMouseButton, typeMouseButton, button);
    cb_event_set_u32(c, kEventParamMouseChord, typeUInt32, (uint32_t)[NSEvent pressedMouseButtons]);
    NSEventType t = [e type];
    if (t != NSEventTypeMouseMoved && t != NSEventTypeScrollWheel)
        cb_event_set_u32(c, kEventParamClickCount, typeUInt32, (uint32_t)[e clickCount]);
}

static void post_raw(CBEvent *c);

// Translate an AppKit event into Carbon events appended to the queue. Returns
// false if the event has no Carbon equivalent (the caller passes it to AppKit).
static bool translate(NSEvent *e) {
    NSEventType t = [e type];
    double when = [e timestamp];
    CBEvent *c = nil;
    switch (t) {
    case NSEventTypeKeyDown:
        keymap_set([e keyCode], true);
        c = cb_event_new(kEventClassKeyboard, [e isARepeat] ? kEventRawKeyRepeat : kEventRawKeyDown, when);
        set_key_params(c, e);
        break;
    case NSEventTypeKeyUp:
        keymap_set([e keyCode], false);
        c = cb_event_new(kEventClassKeyboard, kEventRawKeyUp, when);
        set_key_params(c, e);
        break;
    case NSEventTypeFlagsChanged:
        keymap_update_modifiers([e modifierFlags]);
        c = cb_event_new(kEventClassKeyboard, kEventRawKeyModifiersChanged, when);
        cb_event_set_u32(c, kEventParamKeyModifiers, typeUInt32, cb_modifiers_from_ns([e modifierFlags]));
        cb_event_set_u32(c, kEventParamKeyCode, typeUInt32, [e keyCode]);
        cb_event_set_u32(c, kEventParamKeyboardType, typeUInt32, LMGetKbdType());
        break;
    case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown: {
        // Title bar / frame clicks belong to the window manager (dragging,
        // traffic lights), as with Carbon's standard window handler.
        NSWindow *win = [e window];
        if (win && ([win styleMask] & NSWindowStyleMaskTitled) && !([win styleMask] & NSWindowStyleMaskFullScreen) &&
            !NSPointInRect([e locationInWindow], [[win contentView] frame])) {
            extern void cg_mouse_frame_press(void); // cg_display.c: no mouse grab during the drag
            if ([e type] == NSEventTypeLeftMouseDown) cg_mouse_frame_press();
            return false;
        }
        extern void cg_mouse_content_press(void); // cg_display.c: grab again after a drag
        cg_mouse_content_press();
        c = cb_event_new(kEventClassMouse, kEventMouseDown, when);
        set_mouse_params(c, e);
        break;
    }
    case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
        c = cb_event_new(kEventClassMouse, kEventMouseUp, when);
        set_mouse_params(c, e);
        break;
    case NSEventTypeMouseMoved:
        c = cb_event_new(kEventClassMouse, kEventMouseMoved, when);
        set_mouse_params(c, e);
        break;
    case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        c = cb_event_new(kEventClassMouse, kEventMouseDragged, when);
        set_mouse_params(c, e);
        break;
    case NSEventTypeScrollWheel: {
        double dy = [e scrollingDeltaY], dx = [e scrollingDeltaX];
        if ([e hasPreciseScrollingDeltas]) {
            dy /= 10.0;
            dx /= 10.0;
        }
        int iy = (int)lround(dy), ix = (int)lround(dx);
        if (!iy && dy) iy = dy > 0 ? 1 : -1;
        if (!ix && dx) ix = dx > 0 ? 1 : -1;
        bool any = false;
        if (iy) {
            CBEvent *w = cb_event_new(kEventClassMouse, kEventMouseWheelMoved, when);
            set_mouse_params(w, e);
            cb_event_set_u16(w, kEventParamMouseWheelAxis, typeMouseWheelAxis, kEventMouseWheelAxisY);
            cb_event_set_u32(w, kEventParamMouseWheelDelta, typeSInt32, (uint32_t)iy);
            w->ns = [e retain];
            post_raw(w);
            [w release];
            any = true;
        }
        if (ix) {
            CBEvent *w = cb_event_new(kEventClassMouse, kEventMouseWheelMoved, when);
            set_mouse_params(w, e);
            cb_event_set_u16(w, kEventParamMouseWheelAxis, typeMouseWheelAxis, kEventMouseWheelAxisX);
            cb_event_set_u32(w, kEventParamMouseWheelDelta, typeSInt32, (uint32_t)-ix);
            w->ns = any ? nil : [e retain];
            post_raw(w);
            [w release];
            any = true;
        }
        return any;
    }
    default: return false;
    }
    c->ns = [e retain];
    post_raw(c);
    [c release];
    return true;
}

// --- event queue -----------------------------------------------------------------------------------------

@interface CBQueue : CBBase
@end
@implementation CBQueue
@end

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableArray *queue; // CBEvent, front = next
static CBQueue *main_queue;
static atomic_bool wake_pending;
static atomic_bool app_quit;
static int app_loop_depth;

// Make a pending nextEventMatchingMask (main thread) return, e.g. after a timer
// callback quit the loop or posted an event.
void cb_wake_main(void);
static void wake_main(void);
void cb_wake_main(void) { wake_main(); }
static void wake_main(void) {
    if (atomic_exchange(&wake_pending, true)) return;
    @autoreleasepool {
        NSEvent *w = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                       timestamp:0 windowNumber:0 context:nil subtype:0x4458 data1:0 data2:0];
        [NSApp postEvent:w atStart:NO];
    }
}

static bool is_coalescable(CBEvent *e) {
    return e->cls == kEventClassMouse && (e->kind == kEventMouseMoved || e->kind == kEventMouseDragged);
}

static void post_raw(CBEvent *c) {
    pthread_mutex_lock(&queue_lock);
    if (!queue) queue = [[NSMutableArray alloc] init];
    CBEvent *last = [queue lastObject];
    if (last && is_coalescable(c) && last->cls == c->cls && last->kind == c->kind && last->ns) {
        // merge deltas into the new event and replace the old one
        CBParam *pd = cb_event_param(last, kEventParamMouseDelta), *nd = cb_event_param(c, kEventParamMouseDelta);
        if (pd && nd) {
            float a[2], b[2];
            memcpy(a, pd->data.bytes, 8);
            memcpy(b, nd->data.bytes, 8);
            cb_event_set_hipoint(c, kEventParamMouseDelta, CGPointMake(a[0] + b[0], a[1] + b[1]));
        }
        last->queued = false;
        [queue removeLastObject];
    }
    if (queue.count > 2000) {
        ((CBEvent *)queue[0])->queued = false;
        [queue removeObjectAtIndex:0];
    }
    c->queued = true;
    [queue addObject:c];
    pthread_mutex_unlock(&queue_lock);
}

void cb_post(CBEvent *e) {
    pthread_mutex_lock(&queue_lock);
    if (!queue) queue = [[NSMutableArray alloc] init];
    e->queued = true;
    if (e->priority == kEventPriorityHigh) [queue insertObject:e atIndex:0];
    else [queue addObject:e];
    pthread_mutex_unlock(&queue_lock);
    wake_main();
}

static bool spec_matches(uint32_t n, gaddr_t list, CBEvent *e) {
    if (!n) return true;
    for (uint32_t i = 0; i < n; i++)
        if (gread32(list + 8 * i) == e->cls && gread32(list + 8 * i + 4) == e->kind) return true;
    return false;
}

// Find (and optionally remove) the first queued event matching the list.
static CBEvent *queue_find(uint32_t n, gaddr_t list, bool pull) {
    CBEvent *found = nil;
    pthread_mutex_lock(&queue_lock);
    for (NSUInteger i = 0; i < queue.count; i++) {
        CBEvent *e = queue[i];
        if (spec_matches(n, list, e)) {
            found = [e retain];
            if (pull) {
                e->queued = false;
                [queue removeObjectAtIndex:i];
            }
            break;
        }
    }
    pthread_mutex_unlock(&queue_lock);
    if (found && !pull) [found autorelease];
    return found;
}

// Wait for and process one AppKit event (main thread). remaining < 0: forever.
static void pump(double remaining) {
    cpu_main_heartbeat(); // watchdog: waiting for events is not a hang
    @autoreleasepool {
        NSDate *until = remaining < 0    ? [NSDate distantFuture]
                        : remaining == 0 ? [NSDate distantPast]
                                         : [NSDate dateWithTimeIntervalSinceNow:remaining];
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (!e) return;
        if ([e type] == NSEventTypeApplicationDefined && [e subtype] == 0x4458) {
            atomic_store(&wake_pending, false);
            return;
        }
        if (!translate(e)) [NSApp sendEvent:e];
    }
}

THUNK(GetMainEventQueue) {
    if (!main_queue) main_queue = [[CBQueue alloc] init];
    RET(cb_handle(main_queue));
}

THUNK(PostEventToQueue) {
    CBEvent *e = EV(1);
    if (!e) RET(paramErr);
    if (e->queued) RET(eventAlreadyPostedErr);
    [e retain];
    e->priority = (int)ARG(2);
    cb_post(e);
    [e release]; // the queue holds its own reference
    RET(noErr);
}

// --- event loops --------------------------------------------------------------------------------------

@interface CBLoop : CBBase {
  @public
    CFRunLoopRef rl;
    CFRunLoopSourceRef src; // keeps the run loop from finishing immediately
    atomic_bool quit;
}
@end
@implementation CBLoop
@end

static void dummy_perform(void *info) {}
static pthread_key_t loop_key;
static CBLoop *main_loop;

static CBLoop *loop_new(CFRunLoopRef rl) {
    CBLoop *l = [[CBLoop alloc] init];
    l->rl = (CFRunLoopRef)CFRetain(rl);
    CFRunLoopSourceContext ctx = {0};
    ctx.perform = dummy_perform;
    l->src = CFRunLoopSourceCreate(NULL, 0, &ctx);
    CFRunLoopAddSource(rl, l->src, kCFRunLoopCommonModes);
    return l;
}

static CBLoop *cb_main_loop(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      main_loop = loop_new(CFRunLoopGetMain());
    });
    return main_loop;
}

static CBLoop *current_loop(void) {
    if ([NSThread isMainThread]) return cb_main_loop();
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      pthread_key_create(&loop_key, NULL);
    });
    CBLoop *l = pthread_getspecific(loop_key);
    if (!l) {
        l = loop_new(CFRunLoopGetCurrent());
        pthread_setspecific(loop_key, l);
    }
    return l;
}

THUNK(GetCurrentEventLoop) { RET(cb_handle(current_loop())); }
THUNK(GetMainEventLoop) { RET(cb_handle(cb_main_loop())); }

// Wait on a secondary thread's run loop (timers fire there). Returns true if quit.
static bool run_secondary(CBLoop *l, double timeout) {
    double deadline = timeout < 0 ? INFINITY : cb_now() + timeout;
    for (;;) {
        if (atomic_exchange(&l->quit, false)) return true;
        double rem = deadline - cb_now();
        if (rem <= 0) return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, rem > 1e6 ? 1e6 : rem, true);
    }
}

// stop (optional): checked after every pumped event; returns eventLoopTimedOutErr when it
// becomes true, so dispatch loops notice quit flags set by handlers run from AppKit.
static OSStatus receive_until(uint32_t n, gaddr_t list, double timeout, bool pull, CBEvent **out,
                              bool (^stop)(void)) {
    *out = nil;
    if (![NSThread isMainThread]) {
        // Secondary threads have no events of their own here.
        if (run_secondary(current_loop(), timeout)) return eventLoopQuitErr;
        return eventLoopTimedOutErr;
    }
    cb_app_init();
    double deadline = timeout < 0 ? INFINITY : cb_now() + timeout;
    bool pumped = false;
    for (;;) {
        if (atomic_load(&app_quit) || atomic_exchange(&cb_main_loop()->quit, false)) return eventLoopQuitErr;
        CBEvent *e = queue_find(n, list, pull);
        if (e) {
            *out = e;
            return noErr;
        }
        double rem = timeout < 0 ? -1 : deadline - cb_now();
        if (timeout >= 0 && rem <= 0) {
            if (pumped) return eventLoopTimedOutErr;
            rem = 0;
        }
        pump(rem);
        pumped = true;
        if (stop && stop()) return eventLoopTimedOutErr;
    }
}

static OSStatus receive(uint32_t n, gaddr_t list, double timeout, bool pull, CBEvent **out) {
    return receive_until(n, list, timeout, pull, out, nil);
}

// ReceiveNextEvent(numTypes, list, EventTimeout (double), Boolean pull, EventRef *out)
THUNK(ReceiveNextEvent) {
    CBEvent *e = nil;
    OSStatus st = receive(ARG(0), ARG(1), ARGD(2), ARG(4) & 0xff, &e);
    if (ARG(5)) gwrite32(ARG(5), cb_handle(e));
    RET(st);
}

static OSStatus run_loop_for(double timeout) {
    if (![NSThread isMainThread]) {
        return run_secondary(current_loop(), timeout) ? eventLoopQuitErr : eventLoopTimedOutErr;
    }
    cb_app_init();
    double deadline = timeout < 0 ? INFINITY : cb_now() + timeout;
    bool pumped = false;
    for (;;) {
        if (atomic_load(&app_quit) || atomic_exchange(&cb_main_loop()->quit, false)) return eventLoopQuitErr;
        double rem = timeout < 0 ? -1 : deadline - cb_now();
        if (timeout >= 0 && rem <= 0) {
            if (pumped) return eventLoopTimedOutErr;
            rem = 0;
        }
        pump(rem);
        pumped = true;
    }
}

THUNK(RunCurrentEventLoop) { RET(run_loop_for(ARGD(0))); }

static void dispatch_loop(bool (^stop)(void)) {
    while (!stop()) {
        CBEvent *e = nil;
        OSStatus st = receive_until(0, 0, -1, true, &e, stop);
        if (st == eventLoopQuitErr) break;
        if (e) {
            last_event_mods = 0;
            uint32_t m;
            if (cb_event_get_u32(e, kEventParamKeyModifiers, &m)) last_event_mods = m;
            cb_send(e, cb_dispatcher_target());
            [e release];
        }
    }
}

THUNK(RunApplicationEventLoop) {
    cb_app_init();
    LOGI("carbon: RunApplicationEventLoop");
    cb_app_target()->standard = true;
    app_loop_depth++;
    dispatch_loop(^bool {
      return atomic_load(&app_quit);
    });
    if (--app_loop_depth == 0) atomic_store(&app_quit, false);
    LOGI("carbon: RunApplicationEventLoop returned");
}

void cb_quit_app_loop(void) {
    atomic_store(&app_quit, true);
    wake_main();
}
THUNK(QuitApplicationEventLoop) {
    LOGI("carbon: QuitApplicationEventLoop");
    cb_quit_app_loop();
}

// Run a nested dispatch loop until *flag becomes true (modal loops).
void cb_run_modal(volatile bool *flag) {
    dispatch_loop(^bool {
      return *flag;
    });
}

// --- timers ----------------------------------------------------------------------------------------------

@interface CBTimer : CBBase {
  @public
    CFRunLoopTimerRef t;
    gaddr_t upp, udata;
}
@end
@implementation CBTimer
@end

static void timer_fire(CFRunLoopTimerRef rt, void *info) {
    CBTimer *t = info;
    [t retain];
    LOGT("carbon: timer %#x fires (upp %#x)", cb_handle(t), t->upp);
    gcall2(t->upp, cb_handle(t), t->udata);
    [t release];
}

// InstallEventLoopTimer(loop, double delay, double interval, upp, userData, EventLoopTimerRef *out)
THUNK(InstallEventLoopTimer) {
    CBLoop *l = CB_UNWRAP(CBLoop, ARG(0));
    if (!l) l = current_loop();
    double delay = ARGD(1), interval = ARGD(3);
    CBTimer *t = [[CBTimer alloc] init];
    t->upp = ARG(5);
    t->udata = ARG(6);
    CFRunLoopTimerContext ctx = {0, t, NULL, NULL, NULL};
    CFAbsoluteTime first = delay < 0 ? 1e12 : CFAbsoluteTimeGetCurrent() + delay;
    t->t = CFRunLoopTimerCreate(NULL, first, interval > 0 ? interval : 0, 0, 0, timer_fire, &ctx);
    CFRunLoopAddTimer(l->rl, t->t, kCFRunLoopCommonModes);
    LOGD("carbon: InstallEventLoopTimer(delay %g, interval %g, upp %#x) on %s loop", delay, interval, ARG(5),
         l == main_loop ? "main" : "secondary");
    if (ARG(7)) gwrite32(ARG(7), cb_handle(t));
    RET(noErr);
}

THUNK(RemoveEventLoopTimer) {
    CBTimer *t = CB_UNWRAP(CBTimer, ARG(0));
    if (!t) RET(paramErr);
    if (t->t) {
        CFRunLoopTimerInvalidate(t->t);
        CFRelease(t->t);
        t->t = NULL;
    }
    [t release];
    RET(noErr);
}

THUNK(SetEventLoopTimerNextFireTime) {
    CBTimer *t = CB_UNWRAP(CBTimer, ARG(0));
    if (!t || !t->t) RET(paramErr);
    double d = ARGD(1);
    CFRunLoopTimerSetNextFireDate(t->t, d < 0 ? 1e12 : CFAbsoluteTimeGetCurrent() + d);
    RET(noErr);
}

// --- default handlers ---------------------------------------------------------------------------------------

static CBTarget *focus_target(void) {
    CBWindow *w = cb_key_window();
    if (w) return w->focus ? w->focus->target : w->target;
    return cb_app_target();
}

static OSStatus dispatcher_default(CBEvent *ev, CBTarget *t) {
    CBTarget *dest = cb_app_target();
    if (ev->cls == kEventClassKeyboard || ev->cls == kEventClassTextInput) {
        dest = focus_target();
    } else if (ev->cls == kEventClassMouse) {
        uint32_t wh;
        CBWindow *w = nil;
        if (cb_event_get_u32(ev, kEventParamWindowRef, &wh)) w = CB_UNWRAP(CBWindow, wh);
        if (w) dest = w->target;
    }
    return cb_send(ev, dest);
}

OSStatus cb_send_command(uint32_t command, uint32_t attributes, gaddr_t menu, uint16_t item, gaddr_t control,
                         CBTarget *t) {
    CBEvent *e = cb_event_new(kEventClassCommand, kEventCommandProcess, cb_now());
    uint8_t cmd[14];
    memcpy(cmd, &attributes, 4);
    memcpy(cmd + 4, &command, 4);
    if (control) memcpy(cmd + 8, &control, 4);
    else memcpy(cmd + 8, &menu, 4);
    memcpy(cmd + 12, &item, 2);
    if (control) memset(cmd + 12, 0, 2);
    cb_event_set(e, kEventParamDirectObject, typeHICommand, cmd, 14);
    cb_event_set_u32(e, kEventParamKeyModifiers, typeUInt32, cb_current_modifiers());
    char b[8];
    LOGD("carbon: command '%s' (attrs %#x)", cb_fourcc(command, b), attributes);
    OSStatus st = cb_send(e, t ? t : focus_target());
    [e release];
    return st;
}

static OSStatus app_default(CBEvent *ev, CBTarget *t) {
    if (ev->cls == kEventClassCommand && ev->kind == kEventCommandProcess) {
        CBParam *p = cb_event_param(ev, kEventParamDirectObject);
        if (p && p->data.length >= 8) {
            uint32_t cmd;
            memcpy(&cmd, (const uint8_t *)p->data.bytes + 4, 4);
            if (cmd == kHICommandQuit) {
                cb_quit_app_loop();
                return noErr;
            }
        }
        return eventNotHandledErr;
    }
    if (ev->cls == kEventClassKeyboard && (ev->kind == kEventRawKeyDown || ev->kind == kEventRawKeyRepeat) &&
        ev->ns && !ev->ns_forwarded && !([ev->ns modifierFlags] & NSEventModifierFlagCommand)) {
        // Unhandled raw key: offer it as text input to the focus first.
        CBParam *u = cb_event_param(ev, kEventParamKeyUnicodes);
        if (u && u->data.length) {
            CBEvent *ti = cb_event_new(kEventClassTextInput, kEventTextInputUnicodeForKeyEvent, ev->time);
            cb_event_set(ti, kEventParamTextInputSendText, typeUnicodeText, u->data.bytes, (uint32_t)u->data.length);
            uint32_t eh = cb_handle(ev);
            cb_event_set(ti, kEventParamTextInputSendKeyboardEvent, typeEventRef, &eh, 4);
            ev->ns_forwarded = true; // avoid recursion through our own default
            CBTarget *ft = focus_target();
            // Only guest handlers may consume it: send to the focus chain without the app default
            OSStatus st = cb_send(ti, ft);
            [ti release];
            ev->ns_forwarded = false;
            if (st == noErr) return noErr;
        }
    }
    if (ev->ns && !ev->ns_forwarded) {
        ev->ns_forwarded = true;
        if ([NSThread isMainThread]) [NSApp sendEvent:ev->ns];
        return noErr;
    }
    return eventNotHandledErr;
}

// --- AppKit notifications -> application events ------------------------------------------------------------

static void send_app_event(uint32_t kind) {
    CBEvent *e = cb_event_new(kEventClassApplication, kind, cb_now());
    cb_send(e, cb_app_target());
    [e release];
}

static void app_notifications(void) {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  LOGD("carbon: app activated");
                  send_app_event(kEventAppActivated);
                }];
    [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  LOGD("carbon: app deactivated");
                  extern void cg_mouse_app_deactivated(void); // cg_display.c
                  cg_mouse_app_deactivated();
                  cb_keymap_clear();
                  send_app_event(kEventAppDeactivated);
                }];
    [nc addObserverForName:NSApplicationDidHideNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  send_app_event(kEventAppHidden);
                }];
    [nc addObserverForName:NSApplicationDidUnhideNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                  send_app_event(kEventAppShown);
                }];
}

// --- mouse --------------------------------------------------------------------------------------------------

// HIGetMousePosition(HICoordinateSpace, void *object, HIPoint *out) -> HIPoint *
THUNK(HIGetMousePosition) {
    CGPoint g = cb_ns_to_global([NSEvent mouseLocation]);
    uint32_t space = ARG(0);
    if (space == kHICoordSpaceWindow) {
        CBWindow *w = CB_UNWRAP(CBWindow, ARG(1));
        if (w) {
            CGRect s = cb_window_structure_global(w);
            g.x -= s.origin.x;
            g.y -= s.origin.y;
        }
    } else if (space == kHICoordSpaceView) {
        CBView *v = CB_UNWRAP(CBView, ARG(1));
        if (v && v->window) {
            CGRect s = cb_window_structure_global(v->window);
            g = cb_window_to_view(v, CGPointMake(g.x - s.origin.x, g.y - s.origin.y));
        }
    }
    if (ARG(2)) {
        gwritef(ARG(2), g.x);
        gwritef(ARG(2) + 4, g.y);
    }
    RET(ARG(2));
}

gaddr_t cb_port_window(gaddr_t port, CBWindow **out);

// TrackMouseLocationWithOptions(GrafPtr port, OptionBits, EventTimeout (double), Point *outPt,
//                               UInt32 *outModifiers, MouseTrackingResult *outResult)
THUNK(TrackMouseLocationWithOptions) {
    double timeout = ARGD(2);
    uint16_t result = kMouseTrackingTimedOut;
    NSEvent *e = nil;
    @autoreleasepool {
        NSEventMask mask = NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged | NSEventMaskMouseMoved |
                           NSEventMaskRightMouseUp | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseUp |
                           NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskFlagsChanged;
        NSDate *until = timeout < 0 ? [NSDate distantFuture] : [NSDate dateWithTimeIntervalSinceNow:timeout];
        e = [[NSApp nextEventMatchingMask:mask untilDate:until inMode:NSEventTrackingRunLoopMode dequeue:YES] retain];
    }
    if (e) {
        switch ([e type]) {
        case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
            result = kMouseTrackingMouseUp;
            break;
        case NSEventTypeLeftMouseDown: result = kMouseTrackingMouseDown; break;
        case NSEventTypeMouseMoved: result = kMouseTrackingMouseMoved; break;
        case NSEventTypeFlagsChanged: result = kMouseTrackingKeyModifiersChanged; break;
        default: result = kMouseTrackingMouseDragged; break;
        }
        [e release];
    }
    CGPoint g = cb_ns_to_global([NSEvent mouseLocation]);
    CBWindow *w = nil;
    if (ARG(0)) cb_port_window(ARG(0), &w);
    if (w) {
        CGRect c = cb_window_content_global(w);
        g.x -= c.origin.x;
        g.y -= c.origin.y;
    }
    if (ARG(4)) {
        gwrite16(ARG(4), (uint16_t)(int16_t)lround(g.y));
        gwrite16(ARG(4) + 2, (uint16_t)(int16_t)lround(g.x));
    }
    if (ARG(5)) gwrite32(ARG(5), cb_current_modifiers());
    if (ARG(6)) gwrite16(ARG(6), result);
    RET(noErr);
}
