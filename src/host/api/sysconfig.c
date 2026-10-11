// SystemConfiguration: network reachability and proxy settings.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include "ret_return.h"

__asm__(".linker_option \"-framework\", \"SystemConfiguration\"");

THUNK(SCNetworkReachabilityCreateWithName) {
    LOGI("sc: reachability for \"%s\"", ARGS(1));
    RET_CF(SCNetworkReachabilityCreateWithName(gcf_allocator(ARG(0)), ARGS(1)));
}
// struct sockaddr has the same layout on i386 and x86_64.
THUNK(SCNetworkReachabilityCreateWithAddress) {
    RET_CF(SCNetworkReachabilityCreateWithAddress(gcf_allocator(ARG(0)), ARGP(1)));
}
THUNK(SCNetworkReachabilityGetFlags) {
    SCNetworkReachabilityFlags f = 0;
    Boolean ok = SCNetworkReachabilityGetFlags((SCNetworkReachabilityRef)ARGCF(0), &f);
    gwrite32(ARG(1), f);
    RET_BOOL(ok);
}

struct greach {
    gaddr_t fn;
    struct gcf_context ctx;
};
static char reach_key;
static void reach_free(void *p) {
    struct greach *g = p;
    gcf_context_release(&g->ctx);
    free(g);
}
static void reach_cb(SCNetworkReachabilityRef target, SCNetworkReachabilityFlags flags, void *info) {
    struct greach *g = info;
    gcall3(g->fn, cfg(target), flags, g->ctx.info);
}

// SCNetworkReachabilitySetCallback(target, callout, SCNetworkReachabilityContext *ctx)
THUNK(SCNetworkReachabilitySetCallback) {
    SCNetworkReachabilityRef t = (SCNetworkReachabilityRef)ARGCF(0);
    if (!ARG(1)) {
        Boolean ok = SCNetworkReachabilitySetCallback(t, NULL, NULL);
        cf_attach(t, &reach_key, NULL, NULL);
        RET_BOOL(ok);
    }
    struct greach *g = calloc(1, sizeof *g);
    g->fn = ARG(1);
    gcf_context_read(ARG(2), &g->ctx, true);
    SCNetworkReachabilityContext hc = {0, g, NULL, NULL, NULL};
    Boolean ok = SCNetworkReachabilitySetCallback(t, reach_cb, &hc);
    cf_attach(t, &reach_key, g, reach_free);
    RET_BOOL(ok);
}
THUNK(SCNetworkReachabilityScheduleWithRunLoop) {
    RET_BOOL(SCNetworkReachabilityScheduleWithRunLoop((SCNetworkReachabilityRef)ARGCF(0), (CFRunLoopRef)ARGCF(1),
                                                      ARGSTR(2)));
}
THUNK(SCNetworkReachabilityUnscheduleFromRunLoop) {
    RET_BOOL(SCNetworkReachabilityUnscheduleFromRunLoop((SCNetworkReachabilityRef)ARGCF(0), (CFRunLoopRef)ARGCF(1),
                                                        ARGSTR(2)));
}
THUNK(SCDynamicStoreCopyProxies) { RET_CF(SCDynamicStoreCopyProxies((SCDynamicStoreRef)ARGCF(0))); }
THUNK(SCDynamicStoreCopyComputerName) {
    CFStringEncoding enc = 0;
    CFStringRef s = SCDynamicStoreCopyComputerName((SCDynamicStoreRef)ARGCF(0), &enc);
    if (ARG(1)) gwrite32(ARG(1), enc);
    RET_CF(s);
}
