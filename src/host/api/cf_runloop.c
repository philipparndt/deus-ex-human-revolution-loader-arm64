// CoreFoundation: CFRunLoop (timers, observers, sources) and CFReadStream/CFWriteStream.
//
// Guest callbacks are wrapped in host records passed as the host context's
// info; the host callout calls into the guest with cpu_call (which attaches a
// guest stack to the calling host thread if necessary).
#include "cf_bridge.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define CF_CONST(sym)                                                                              \
    static void init_cfc_##sym(gaddr_t a) { gwrite32(a, cfg((CFTypeRef)(sym))); }                \
    GDATA(sym, 4, init_cfc_##sym)

CF_CONST(kCFRunLoopDefaultMode);
CF_CONST(kCFRunLoopCommonModes);
// NSRunLoopCommonModes / NSDefaultRunLoopMode are the same strings (toll-free bridged).
static void init_ns_common(gaddr_t a) { gwrite32(a, cfg(kCFRunLoopCommonModes)); }
static void init_ns_default(gaddr_t a) { gwrite32(a, cfg(kCFRunLoopDefaultMode)); }
GDATA(NSRunLoopCommonModes, 4, init_ns_common);
GDATA(NSDefaultRunLoopMode, 4, init_ns_default);

#define RL(i) ((CFRunLoopRef)ARGCF(i))
#define MODE(i) ARGSTR(i)

// --- run loops ---------------------------------------------------------------------------
THUNK(CFRunLoopGetTypeID) { RET(CFRunLoopGetTypeID()); }
THUNK(CFRunLoopGetCurrent) { RET_CF(CFRunLoopGetCurrent()); }
THUNK(CFRunLoopGetMain) { RET_CF(CFRunLoopGetMain()); }
THUNK(CFRunLoopRun) { CFRunLoopRun(); }
THUNK(CFRunLoopRunInMode) { RET((uint32_t)CFRunLoopRunInMode(MODE(0), ARGD(1), ARGBOOL(3))); }
THUNK(CFRunLoopStop) {
    if (RL(0) == CFRunLoopGetMain()) {
        extern void cb_stop_main_loop(void); // carbon_event.m
        static int logged;
        if (logged++ < 3) LOGI("cf: CFRunLoopStop(main run loop): quitting the main thread's event loop");
        cb_stop_main_loop();
    }
    CFRunLoopStop(RL(0));
}
THUNK(CFRunLoopWakeUp) { CFRunLoopWakeUp(RL(0)); }
THUNK(CFRunLoopIsWaiting) { RET_BOOL(CFRunLoopIsWaiting(RL(0))); }
THUNK(CFRunLoopCopyCurrentMode) { RET_CF(CFRunLoopCopyCurrentMode(RL(0))); }
THUNK(CFRunLoopCopyAllModes) { RET_CF(CFRunLoopCopyAllModes(RL(0))); }
THUNK(CFRunLoopAddCommonMode) { CFRunLoopAddCommonMode(RL(0), MODE(1)); }
THUNK(CFRunLoopGetNextTimerFireDate) { RETD(CFRunLoopGetNextTimerFireDate(RL(0), MODE(1))); }
THUNK(CFRunLoopAddSource) { CFRunLoopAddSource(RL(0), (CFRunLoopSourceRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopRemoveSource) { CFRunLoopRemoveSource(RL(0), (CFRunLoopSourceRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopContainsSource) { RET_BOOL(CFRunLoopContainsSource(RL(0), (CFRunLoopSourceRef)ARGCF(1), MODE(2))); }
THUNK(CFRunLoopAddTimer) { CFRunLoopAddTimer(RL(0), (CFRunLoopTimerRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopRemoveTimer) { CFRunLoopRemoveTimer(RL(0), (CFRunLoopTimerRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopContainsTimer) { RET_BOOL(CFRunLoopContainsTimer(RL(0), (CFRunLoopTimerRef)ARGCF(1), MODE(2))); }
THUNK(CFRunLoopAddObserver) { CFRunLoopAddObserver(RL(0), (CFRunLoopObserverRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopRemoveObserver) { CFRunLoopRemoveObserver(RL(0), (CFRunLoopObserverRef)ARGCF(1), MODE(2)); }
THUNK(CFRunLoopContainsObserver) {
    RET_BOOL(CFRunLoopContainsObserver(RL(0), (CFRunLoopObserverRef)ARGCF(1), MODE(2)));
}

// Generic record for a guest callout + guest context.
struct gcallout {
    gaddr_t fn;
    struct gcf_context ctx;
    int refs;
};
static struct gcallout *new_callout(gaddr_t fn, gaddr_t ctx) {
    struct gcallout *c = calloc(1, sizeof *c);
    c->fn = fn;
    gcf_context_read(ctx, &c->ctx, true);
    return c;
}
static void callout_release(const void *info) {
    struct gcallout *c = (struct gcallout *)info;
    gcf_context_release(&c->ctx);
    free(c);
}
// Write back the guest view of a host context created by us (info = gcallout).
static void put_callout_context(gaddr_t g, void *info, CFAllocatorReleaseCallBack rel) {
    struct gcf_context gc = {0};
    if (rel == callout_release && info) gc = ((struct gcallout *)info)->ctx;
    else gc.info = (uint32_t)(uintptr_t)info; // not ours: should not happen for guest objects
    gcf_context_write(g, &gc);
}

// --- timers ------------------------------------------------------------------------------------
static void timer_cb(CFRunLoopTimerRef t, void *info) {
    struct gcallout *c = info;
    gcall2(c->fn, cfg(t), c->ctx.info);
}
THUNK(CFRunLoopTimerGetTypeID) { RET(CFRunLoopTimerGetTypeID()); }
THUNK(CFRunLoopTimerCreate) {
    // (alloc, fireDate, interval, flags, order, callout, context*)
    struct gcallout *c = new_callout(ARG(7), ARG(8));
    CFRunLoopTimerContext hc = {0, c, NULL, callout_release, NULL};
    RET_CF(CFRunLoopTimerCreate(gcf_allocator(ARG(0)), ARGD(1), ARGD(3), ARG(5), ARGIDX(6), timer_cb, &hc));
}
THUNK(CFRunLoopTimerGetContext) {
    CFRunLoopTimerContext hc = {0};
    CFRunLoopTimerGetContext((CFRunLoopTimerRef)ARGCF(0), &hc);
    put_callout_context(ARG(1), hc.info, hc.release);
}
THUNK(CFRunLoopTimerInvalidate) { CFRunLoopTimerInvalidate((CFRunLoopTimerRef)ARGCF(0)); }
THUNK(CFRunLoopTimerIsValid) { RET_BOOL(CFRunLoopTimerIsValid((CFRunLoopTimerRef)ARGCF(0))); }
THUNK(CFRunLoopTimerDoesRepeat) { RET_BOOL(CFRunLoopTimerDoesRepeat((CFRunLoopTimerRef)ARGCF(0))); }
THUNK(CFRunLoopTimerGetInterval) { RETD(CFRunLoopTimerGetInterval((CFRunLoopTimerRef)ARGCF(0))); }
THUNK(CFRunLoopTimerGetNextFireDate) { RETD(CFRunLoopTimerGetNextFireDate((CFRunLoopTimerRef)ARGCF(0))); }
THUNK(CFRunLoopTimerSetNextFireDate) { CFRunLoopTimerSetNextFireDate((CFRunLoopTimerRef)ARGCF(0), ARGD(1)); }
THUNK(CFRunLoopTimerGetOrder) { RET((uint32_t)CFRunLoopTimerGetOrder((CFRunLoopTimerRef)ARGCF(0))); }
THUNK(CFRunLoopTimerSetTolerance) { CFRunLoopTimerSetTolerance((CFRunLoopTimerRef)ARGCF(0), ARGD(1)); }

// --- observers ----------------------------------------------------------------------------------
static void observer_cb(CFRunLoopObserverRef o, CFRunLoopActivity act, void *info) {
    struct gcallout *c = info;
    gcall3(c->fn, cfg(o), (uint32_t)act, c->ctx.info);
}
THUNK(CFRunLoopObserverGetTypeID) { RET(CFRunLoopObserverGetTypeID()); }
THUNK(CFRunLoopObserverCreate) {
    // (alloc, activities, repeats, order, callout, context*)
    struct gcallout *c = new_callout(ARG(4), ARG(5));
    CFRunLoopObserverContext hc = {0, c, NULL, callout_release, NULL};
    RET_CF(CFRunLoopObserverCreate(gcf_allocator(ARG(0)), ARG(1), ARGBOOL(2), ARGIDX(3), observer_cb, &hc));
}
THUNK(CFRunLoopObserverGetContext) {
    CFRunLoopObserverContext hc = {0};
    CFRunLoopObserverGetContext((CFRunLoopObserverRef)ARGCF(0), &hc);
    put_callout_context(ARG(1), hc.info, hc.release);
}
THUNK(CFRunLoopObserverInvalidate) { CFRunLoopObserverInvalidate((CFRunLoopObserverRef)ARGCF(0)); }
THUNK(CFRunLoopObserverIsValid) { RET_BOOL(CFRunLoopObserverIsValid((CFRunLoopObserverRef)ARGCF(0))); }

// --- sources ------------------------------------------------------------------------------------
// CFRunLoopSourceContext (version 0, i386, 40 bytes): version, info, retain,
// release, copyDescription, equal, hash, schedule, cancel, perform.
struct gsource {
    struct gcf_context ctx;
    gaddr_t equal, hash, schedule, cancel, perform;
};
static void src_release(const void *info) {
    struct gsource *s = (struct gsource *)info;
    gcf_context_release(&s->ctx);
    free(s);
}
static Boolean src_equal(const void *a, const void *b) {
    const struct gsource *x = a, *y = b;
    if (x->equal) return (gcall2(x->equal, x->ctx.info, y->ctx.info) & 0xff) != 0;
    return x->ctx.info == y->ctx.info;
}
static CFHashCode src_hash(const void *info) {
    const struct gsource *s = info;
    return s->hash ? gcall1(s->hash, s->ctx.info) : s->ctx.info;
}
static void src_schedule(void *info, CFRunLoopRef rl, CFRunLoopMode mode) {
    struct gsource *s = info;
    if (s->schedule) gcall3(s->schedule, s->ctx.info, cfg(rl), cfg(mode));
}
static void src_cancel(void *info, CFRunLoopRef rl, CFRunLoopMode mode) {
    struct gsource *s = info;
    if (s->cancel) gcall3(s->cancel, s->ctx.info, cfg(rl), cfg(mode));
}
static void src_perform(void *info) {
    struct gsource *s = info;
    if (s->perform) gcall1(s->perform, s->ctx.info);
}
THUNK(CFRunLoopSourceGetTypeID) { RET(CFRunLoopSourceGetTypeID()); }
THUNK(CFRunLoopSourceCreate) {
    gaddr_t g = ARG(2);
    if (!g) {
        RET(0);
        return;
    }
    if (gread32(g) != 0) fatal("CFRunLoopSourceCreate: version %u (mach port) sources are not supported", gread32(g));
    struct gsource *s = calloc(1, sizeof *s);
    gcf_context_read(g, &s->ctx, true);
    s->equal = gread32(g + 20);
    s->hash = gread32(g + 24);
    s->schedule = gread32(g + 28);
    s->cancel = gread32(g + 32);
    s->perform = gread32(g + 36);
    CFRunLoopSourceContext hc = {0, s, NULL, src_release, NULL, src_equal, src_hash, src_schedule, src_cancel, src_perform};
    RET_CF(CFRunLoopSourceCreate(gcf_allocator(ARG(0)), ARGIDX(1), &hc));
}
THUNK(CFRunLoopSourceGetContext) {
    CFRunLoopSourceContext hc = {0};
    CFRunLoopSourceGetContext((CFRunLoopSourceRef)ARGCF(0), &hc);
    gaddr_t g = ARG(1);
    if (hc.release == src_release) {
        struct gsource *s = hc.info;
        gcf_context_write(g, &s->ctx);
        gwrite32(g + 20, s->equal);
        gwrite32(g + 24, s->hash);
        gwrite32(g + 28, s->schedule);
        gwrite32(g + 32, s->cancel);
        gwrite32(g + 36, s->perform);
    } else {
        LOGE("CFRunLoopSourceGetContext on a host-created source");
        memset(g2h(g), 0, 40);
    }
}
THUNK(CFRunLoopSourceInvalidate) { CFRunLoopSourceInvalidate((CFRunLoopSourceRef)ARGCF(0)); }
THUNK(CFRunLoopSourceIsValid) { RET_BOOL(CFRunLoopSourceIsValid((CFRunLoopSourceRef)ARGCF(0))); }
THUNK(CFRunLoopSourceSignal) { CFRunLoopSourceSignal((CFRunLoopSourceRef)ARGCF(0)); }
THUNK(CFRunLoopSourceGetOrder) { RET((uint32_t)CFRunLoopSourceGetOrder((CFRunLoopSourceRef)ARGCF(0))); }

// --- streams --------------------------------------------------------------------------------------
#define RS(i) ((CFReadStreamRef)ARGCF(i))
#define WS(i) ((CFWriteStreamRef)ARGCF(i))

static void read_client_cb(CFReadStreamRef s, CFStreamEventType ev, void *info) {
    struct gcallout *c = info;
    gcall3(c->fn, cfg(s), (uint32_t)ev, c->ctx.info);
}
static void write_client_cb(CFWriteStreamRef s, CFStreamEventType ev, void *info) {
    struct gcallout *c = info;
    gcall3(c->fn, cfg(s), (uint32_t)ev, c->ctx.info);
}
static void *stream_retain(void *info) { __atomic_add_fetch(&((struct gcallout *)info)->refs, 1, __ATOMIC_SEQ_CST); return info; }
static void stream_release(void *info) { if (__atomic_sub_fetch(&((struct gcallout *)info)->refs, 1, __ATOMIC_SEQ_CST) <= 0) callout_release(info); }
// CFStreamError (i386): {CFIndex domain; SInt32 error} = 8 bytes, returned in edx:eax.
#define RET_STREAMERROR(e)                                                                         \
    do {                                                                                           \
        CFStreamError e_ = (e);                                                                    \
        RET64((uint64_t)(uint32_t)e_.domain | ((uint64_t)(uint32_t)e_.error << 32));               \
    } while (0)

THUNK(CFReadStreamGetTypeID) { RET(CFReadStreamGetTypeID()); }
THUNK(CFReadStreamCreateWithFile) { RET_CF(CFReadStreamCreateWithFile(gcf_allocator(ARG(0)), (CFURLRef)ARGCF(1))); }
THUNK(CFReadStreamCreateWithBytesNoCopy) {
    RET_CF(CFReadStreamCreateWithBytesNoCopy(gcf_allocator(ARG(0)), ARGP(1), ARGIDX(2), cf_guest_deallocator(ARG(3))));
}
THUNK(CFReadStreamOpen) { RET_BOOL(CFReadStreamOpen(RS(0))); }
THUNK(CFReadStreamClose) { CFReadStreamClose(RS(0)); }
THUNK(CFReadStreamRead) { RET((uint32_t)CFReadStreamRead(RS(0), ARGP(1), ARGIDX(2))); }
THUNK(CFReadStreamHasBytesAvailable) { RET_BOOL(CFReadStreamHasBytesAvailable(RS(0))); }
THUNK(CFReadStreamGetStatus) { RET((uint32_t)CFReadStreamGetStatus(RS(0))); }
THUNK(CFReadStreamGetError) { RET_STREAMERROR(CFReadStreamGetError(RS(0))); }
THUNK(CFReadStreamCopyError) { RET_CF(CFReadStreamCopyError(RS(0))); }
THUNK(CFReadStreamCopyProperty) { RET_CF(CFReadStreamCopyProperty(RS(0), ARGSTR(1))); }
THUNK(CFReadStreamSetProperty) { RET_BOOL(CFReadStreamSetProperty(RS(0), ARGSTR(1), ARGCF(2))); }
THUNK(CFReadStreamScheduleWithRunLoop) { CFReadStreamScheduleWithRunLoop(RS(0), RL(1), MODE(2)); }
THUNK(CFReadStreamUnscheduleFromRunLoop) { CFReadStreamUnscheduleFromRunLoop(RS(0), RL(1), MODE(2)); }
THUNK(CFReadStreamSetClient) {
    // (stream, flags, callback, CFStreamClientContext*)
    if (!ARG(2)) {
        RET_BOOL(CFReadStreamSetClient(RS(0), ARG(1), NULL, NULL));
        return;
    }
    struct gcallout *c = new_callout(ARG(2), ARG(3));
    CFStreamClientContext hc = {0, c, stream_retain, stream_release, NULL};
    RET_BOOL(CFReadStreamSetClient(RS(0), ARG(1), read_client_cb, &hc));
}

THUNK(CFWriteStreamGetTypeID) { RET(CFWriteStreamGetTypeID()); }
THUNK(CFWriteStreamCreateWithFile) { RET_CF(CFWriteStreamCreateWithFile(gcf_allocator(ARG(0)), (CFURLRef)ARGCF(1))); }
THUNK(CFWriteStreamCreateWithAllocatedBuffers) {
    RET_CF(CFWriteStreamCreateWithAllocatedBuffers(gcf_allocator(ARG(0)), gcf_allocator(ARG(1))));
}
THUNK(CFWriteStreamOpen) { RET_BOOL(CFWriteStreamOpen(WS(0))); }
THUNK(CFWriteStreamClose) { CFWriteStreamClose(WS(0)); }
THUNK(CFWriteStreamWrite) { RET((uint32_t)CFWriteStreamWrite(WS(0), ARGP(1), ARGIDX(2))); }
THUNK(CFWriteStreamCanAcceptBytes) { RET_BOOL(CFWriteStreamCanAcceptBytes(WS(0))); }
THUNK(CFWriteStreamGetStatus) { RET((uint32_t)CFWriteStreamGetStatus(WS(0))); }
THUNK(CFWriteStreamGetError) { RET_STREAMERROR(CFWriteStreamGetError(WS(0))); }
THUNK(CFWriteStreamCopyError) { RET_CF(CFWriteStreamCopyError(WS(0))); }
THUNK(CFWriteStreamCopyProperty) { RET_CF(CFWriteStreamCopyProperty(WS(0), ARGSTR(1))); }
THUNK(CFWriteStreamSetProperty) { RET_BOOL(CFWriteStreamSetProperty(WS(0), ARGSTR(1), ARGCF(2))); }
THUNK(CFWriteStreamScheduleWithRunLoop) { CFWriteStreamScheduleWithRunLoop(WS(0), RL(1), MODE(2)); }
THUNK(CFWriteStreamUnscheduleFromRunLoop) { CFWriteStreamUnscheduleFromRunLoop(WS(0), RL(1), MODE(2)); }
THUNK(CFWriteStreamSetClient) {
    if (!ARG(2)) {
        RET_BOOL(CFWriteStreamSetClient(WS(0), ARG(1), NULL, NULL));
        return;
    }
    struct gcallout *c = new_callout(ARG(2), ARG(3));
    CFStreamClientContext hc = {0, c, stream_retain, stream_release, NULL};
    RET_BOOL(CFWriteStreamSetClient(WS(0), ARG(1), write_client_cb, &hc));
}
THUNK(CFStreamCreatePairWithSocketToHost) {
    // (alloc, host, UInt32 port, CFReadStreamRef *, CFWriteStreamRef *)
    CFReadStreamRef rs = NULL;
    CFWriteStreamRef ws = NULL;
    CFStreamCreatePairWithSocketToHost(gcf_allocator(ARG(0)), ARGSTR(1), ARG(2), ARG(3) ? &rs : NULL,
                                       ARG(4) ? &ws : NULL);
    if (ARG(3)) gwrite32(ARG(3), cfg(rs));
    if (ARG(4)) gwrite32(ARG(4), cfg(ws));
}
