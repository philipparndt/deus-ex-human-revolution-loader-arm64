// CFNetwork (exported through CoreServices): CFHTTPMessage, proxies, HTTP
// read streams, plus CoreServices CF string constants (UTIs, FSFileOperation keys).
#include "cf_bridge.h"

#include <CFNetwork/CFNetwork.h>
#include <CoreServices/CoreServices.h>

__asm__(".linker_option \"-framework\", \"CFNetwork\"");
__asm__(".linker_option \"-framework\", \"CoreServices\"");

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// --- CFHTTPMessage ------------------------------------------------------------------------
THUNK(CFHTTPMessageCreateRequest) {
    RET_CF(CFHTTPMessageCreateRequest(gcf_allocator(ARG(0)), ARGSTR(1), (CFURLRef)ARGCF(2), ARGSTR(3)));
}
THUNK(CFHTTPMessageCreateResponse) {
    RET_CF(CFHTTPMessageCreateResponse(gcf_allocator(ARG(0)), ARGIDX(1), ARGSTR(2), ARGSTR(3)));
}
THUNK(CFHTTPMessageCreateEmpty) { RET_CF(CFHTTPMessageCreateEmpty(gcf_allocator(ARG(0)), ARGBOOL(1))); }
THUNK(CFHTTPMessageAppendBytes) {
    RET_BOOL(CFHTTPMessageAppendBytes((CFHTTPMessageRef)ARGCF(0), (const UInt8 *)ARGP(1), ARGIDX(2)));
}
THUNK(CFHTTPMessageCopyAllHeaderFields) { RET_CF(CFHTTPMessageCopyAllHeaderFields((CFHTTPMessageRef)ARGCF(0))); }
THUNK(CFHTTPMessageCopyBody) { RET_CF(CFHTTPMessageCopyBody((CFHTTPMessageRef)ARGCF(0))); }
THUNK(CFHTTPMessageCopyHeaderFieldValue) {
    RET_CF(CFHTTPMessageCopyHeaderFieldValue((CFHTTPMessageRef)ARGCF(0), ARGSTR(1)));
}
THUNK(CFHTTPMessageGetResponseStatusCode) {
    RET((uint32_t)CFHTTPMessageGetResponseStatusCode((CFHTTPMessageRef)ARGCF(0)));
}
THUNK(CFHTTPMessageIsHeaderComplete) { RET_BOOL(CFHTTPMessageIsHeaderComplete((CFHTTPMessageRef)ARGCF(0))); }
THUNK(CFHTTPMessageSetBody) { CFHTTPMessageSetBody((CFHTTPMessageRef)ARGCF(0), (CFDataRef)ARGCF(1)); }
THUNK(CFHTTPMessageSetHeaderFieldValue) {
    CFHTTPMessageSetHeaderFieldValue((CFHTTPMessageRef)ARGCF(0), ARGSTR(1), ARGSTR(2));
}
THUNK(CFHTTPMessageCopySerializedMessage) { RET_CF(CFHTTPMessageCopySerializedMessage((CFHTTPMessageRef)ARGCF(0))); }
THUNK(CFHTTPMessageCopyRequestURL) { RET_CF(CFHTTPMessageCopyRequestURL((CFHTTPMessageRef)ARGCF(0))); }

THUNK(CFReadStreamCreateForHTTPRequest) {
    RET_CF(CFReadStreamCreateForHTTPRequest(gcf_allocator(ARG(0)), (CFHTTPMessageRef)ARGCF(1)));
}

// --- proxies ----------------------------------------------------------------------------------
THUNK(CFNetworkCopyProxiesForURL) { RET_CF(CFNetworkCopyProxiesForURL((CFURLRef)ARGCF(0), (CFDictionaryRef)ARGCF(1))); }
THUNK(CFNetworkCopySystemProxySettings) { RET_CF(CFNetworkCopySystemProxySettings()); }

struct pac_ctx {
    gaddr_t cb;
    struct gcf_context g;
};

static void pac_result(void *client, CFArrayRef proxies, CFErrorRef error) {
    struct pac_ctx *c = client;
    if (c->cb) gcall3(c->cb, c->g.info, cfg(proxies), cfg(error));
}

static void pac_release(void *info) {
    struct pac_ctx *c = info;
    gcf_context_release(&c->g);
    free(c);
}

// CFNetworkExecuteProxyAutoConfigurationURL(pacURL, targetURL, cb, CFStreamClientContext *ctx)
THUNK(CFNetworkExecuteProxyAutoConfigurationURL) {
    struct pac_ctx *c = calloc(1, sizeof *c);
    c->cb = ARG(2);
    gcf_context_read(ARG(3), &c->g, true);
    CFStreamClientContext hc = {0, c, NULL, pac_release, NULL};
    CFRunLoopSourceRef src =
        CFNetworkExecuteProxyAutoConfigurationURL((CFURLRef)ARGCF(0), (CFURLRef)ARGCF(1), pac_result, &hc);
    if (!src) pac_release(c);
    RET_CF(src);
}

// --- constants -----------------------------------------------------------------------------------
#define CF_CONST(sym)                                                                              \
    static void init_##sym(gaddr_t a) { gwrite32(a, cfg(sym)); }                                    \
    GDATA(sym, 4, init_##sym)

CF_CONST(kCFHTTPVersion1_0);
CF_CONST(kCFHTTPVersion1_1);
CF_CONST(kCFProxyAutoConfigurationURLKey);
CF_CONST(kCFProxyHostNameKey);
CF_CONST(kCFProxyPortNumberKey);
CF_CONST(kCFProxyTypeAutoConfigurationURL);
CF_CONST(kCFProxyTypeHTTP);
CF_CONST(kCFProxyTypeHTTPS);
CF_CONST(kCFProxyTypeNone);
CF_CONST(kCFProxyTypeSOCKS);
CF_CONST(kCFProxyTypeKey);
CF_CONST(kCFProxyUsernameKey);
CF_CONST(kCFProxyPasswordKey);
CF_CONST(kCFStreamPropertyHTTPProxy);
CF_CONST(kCFStreamPropertyHTTPProxyHost);
CF_CONST(kCFStreamPropertyHTTPProxyPort);
CF_CONST(kCFStreamPropertyHTTPResponseHeader);
CF_CONST(kCFStreamPropertyHTTPShouldAutoredirect);
CF_CONST(kCFStreamPropertyHTTPFinalURL);
CF_CONST(kCFNetworkProxiesHTTPEnable);
CF_CONST(kCFNetworkProxiesHTTPProxy);
CF_CONST(kCFNetworkProxiesHTTPPort);
CF_CONST(kFSOperationBytesCompleteKey);
CF_CONST(kFSOperationTotalBytesKey);
CF_CONST(kFSOperationObjectsCompleteKey);
CF_CONST(kFSOperationTotalObjectsKey);
CF_CONST(kUTTypeJPEG);
CF_CONST(kUTTypeTIFF);
CF_CONST(kUTTypePNG);
CF_CONST(kUTTypeBMP);
CF_CONST(kUTTypeGIF);
