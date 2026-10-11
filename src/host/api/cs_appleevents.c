// AppleEvent Manager (AE.framework) thunks. See cs_ae.h for the AEDesc mapping.
#include "cf_bridge.h"
#include "cs_ae.h"

#include <pthread.h>

__asm__(".linker_option \"-framework\", \"CoreServices\"");

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define GRET_ERR(e) RET((uint32_t)(int32_t)(e))

// Run a host AE call that fills a result AEDesc and copy it to the guest.
#define WITH_RESULT(gptr, call)                                                                    \
    do {                                                                                           \
        AEDesc res_ = {typeNull, NULL};                                                            \
        OSErr e_ = (call);                                                                         \
        if (gptr) {                                                                                \
            if (e_ == noErr) aedesc_to_guest(gptr, &res_);                                         \
            else {                                                                                 \
                gwrite32(gptr, typeNull);                                                          \
                gwrite32((gptr) + 4, 0);                                                           \
            }                                                                                      \
        }                                                                                          \
        GRET_ERR(e_);                                                                              \
    } while (0)

THUNK(AECreateDesc) {
    gaddr_t out = ARG(3);
    WITH_RESULT(out, AECreateDesc(ARG(0), ARGP(1), ARGI(2), &res_));
}

THUNK(AEDisposeDesc) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEDisposeDesc(&d);
    if (ARG(0)) {
        hobj_forget(gread32(ARG(0) + 4));
        gwrite32(ARG(0), typeNull);
        gwrite32(ARG(0) + 4, 0);
    }
    GRET_ERR(e);
}

THUNK(AEDuplicateDesc) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    gaddr_t out = ARG(1);
    WITH_RESULT(out, AEDuplicateDesc(&d, &res_));
}

THUNK(AECoerceDesc) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    gaddr_t out = ARG(2);
    WITH_RESULT(out, AECoerceDesc(&d, ARG(1), &res_));
}

THUNK(AEGetDescDataSize) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    RET((uint32_t)AEGetDescDataSize(&d));
}

THUNK(AEGetDescData) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    GRET_ERR(AEGetDescData(&d, ARGP(1), ARGI(2)));
}

THUNK(AECountItems) {
    AEDesc d;
    long n = 0;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AECountItems(&d, &n);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)n);
    GRET_ERR(e);
}

// AEGetNthDesc(list, long index, desiredType, AEKeyword *keyword, AEDesc *result)
THUNK(AEGetNthDesc) {
    AEDesc d;
    AEKeyword kw = 0;
    aedesc_from_guest(&d, ARG(0));
    gaddr_t out = ARG(4);
    AEDesc res = {typeNull, NULL};
    OSErr e = AEGetNthDesc(&d, ARGI(1), ARG(2), &kw, &res);
    if (e == noErr) {
        if (ARG(3)) gwrite32(ARG(3), kw);
        aedesc_to_guest(out, &res);
    }
    GRET_ERR(e);
}

// AEGetNthPtr(list, index, desiredType, AEKeyword*, DescType*, void*, Size max, Size *actual)
THUNK(AEGetNthPtr) {
    AEDesc d;
    AEKeyword kw = 0;
    DescType t = 0;
    Size actual = 0;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEGetNthPtr(&d, ARGI(1), ARG(2), &kw, &t, ARGP(5), ARGI(6), &actual);
    if (ARG(3)) gwrite32(ARG(3), kw);
    if (ARG(4)) gwrite32(ARG(4), t);
    if (ARG(7)) gwrite32(ARG(7), (uint32_t)actual);
    GRET_ERR(e);
}

THUNK(AEGetParamDesc) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    gaddr_t out = ARG(3);
    WITH_RESULT(out, AEGetParamDesc(&d, ARG(1), ARG(2), &res_));
}

// AEGetParamPtr(evt, keyword, desiredType, DescType *actualType, void *data, Size max, Size *actualSize)
THUNK(AEGetParamPtr) {
    AEDesc d;
    DescType t = 0;
    Size actual = 0;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEGetParamPtr(&d, ARG(1), ARG(2), &t, ARGP(4), ARGI(5), &actual);
    if (ARG(3)) gwrite32(ARG(3), t);
    if (ARG(6)) gwrite32(ARG(6), (uint32_t)actual);
    GRET_ERR(e);
}

THUNK(AEGetAttributePtr) {
    AEDesc d;
    DescType t = 0;
    Size actual = 0;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEGetAttributePtr(&d, ARG(1), ARG(2), &t, ARGP(4), ARGI(5), &actual);
    if (ARG(3)) gwrite32(ARG(3), t);
    if (ARG(6)) gwrite32(ARG(6), (uint32_t)actual);
    GRET_ERR(e);
}

THUNK(AEGetAttributeDesc) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    gaddr_t out = ARG(3);
    WITH_RESULT(out, AEGetAttributeDesc(&d, ARG(1), ARG(2), &res_));
}

// The host modifies the descriptor's storage in place; write back in case the
// storage pointer changed.
THUNK(AEPutParamPtr) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEPutParamPtr(&d, ARG(1), ARG(2), ARGP(3), ARGI(4));
    aedesc_to_guest(ARG(0), &d);
    GRET_ERR(e);
}

THUNK(AEPutParamDesc) {
    AEDesc d, v;
    aedesc_from_guest(&d, ARG(0));
    aedesc_from_guest(&v, ARG(2));
    OSErr e = AEPutParamDesc(&d, ARG(1), &v);
    aedesc_to_guest(ARG(0), &d);
    GRET_ERR(e);
}

THUNK(AEPutDesc) {
    AEDesc d, v;
    aedesc_from_guest(&d, ARG(0));
    aedesc_from_guest(&v, ARG(2));
    OSErr e = AEPutDesc(&d, ARGI(1), &v);
    aedesc_to_guest(ARG(0), &d);
    GRET_ERR(e);
}

THUNK(AEPutPtr) {
    AEDesc d;
    aedesc_from_guest(&d, ARG(0));
    OSErr e = AEPutPtr(&d, ARGI(1), ARG(2), ARGP(3), ARGI(4));
    aedesc_to_guest(ARG(0), &d);
    GRET_ERR(e);
}

// AECreateList(const void *factoringPtr, Size factoredSize, Boolean isRecord, AEDescList *result)
THUNK(AECreateList) {
    gaddr_t out = ARG(3);
    WITH_RESULT(out, AECreateList(ARGP(0), ARGI(1), ARGBOOL(2), &res_));
}

// AECreateAppleEvent(class, id, const AEAddressDesc *target, AEReturnID, AETransactionID, AppleEvent *result)
THUNK(AECreateAppleEvent) {
    AEDesc target;
    aedesc_from_guest(&target, ARG(2));
    gaddr_t out = ARG(5);
    WITH_RESULT(out, AECreateAppleEvent(ARG(0), ARG(1), ARG(2) ? &target : NULL, (AEReturnID)ARG(3), ARGI(4), &res_));
}

// AEBuildAppleEvent(class, id, addressType, addressData, Size addressLength, SInt16 returnID,
//                   SInt32 transactionID, AppleEvent *result, AEBuildError *error, const char *fmt, ...)
// Substitutions in the format (TN2045): a bare "@" takes an AEDesc*; "type(@)" takes
// (const void *data, Size length) (verified on the host); "type(@@)" takes a Handle (unsupported).
#define AEB_MAX_ARGS 24
THUNK(AEBuildAppleEvent) {
    const char *fmt = ARGS(9);
    uintptr_t a[AEB_MAX_ARGS] = {0};
    AEDesc descs[AEB_MAX_ARGS];
    int na = 0, nd = 0;
    struct gva va = GVA_FROM_ARG(10);
    char prev = 0;
    for (const unsigned char *p = (const unsigned char *)fmt; p && *p; p++) {
        unsigned char c = *p;
        if (c == '\'') { // OSType literal
            while (p[1] && p[1] != '\'') p++;
            if (p[1]) p++;
            prev = '\'';
            continue;
        }
        if (c == 0xD2) { // MacRoman curly-quoted string
            while (p[1] && p[1] != 0xD3) p++;
            if (p[1]) p++;
            prev = '"';
            continue;
        }
        if (c == 0xC7) { // «hex»
            while (p[1] && p[1] != 0xC8) p++;
            if (p[1]) p++;
            prev = 'x';
            continue;
        }
        if (c == '@') {
            if (p[1] == '@') { // type(@@): Handle
                LOGE("AEBuildAppleEvent: '@@' (Handle) substitution unsupported in \"%s\"", fmt);
                if (na < AEB_MAX_ARGS) a[na++] = 0;
                gva_u32(&va);
                p++;
            } else if (prev == '(') {
                if (na + 2 > AEB_MAX_ARGS) break;
                a[na++] = (uintptr_t)g2h(gva_u32(&va));              // data
                a[na++] = (uintptr_t)(intptr_t)(int32_t)gva_u32(&va); // Size
            } else {
                if (na >= AEB_MAX_ARGS) break;
                aedesc_from_guest(&descs[nd], gva_u32(&va));
                a[na++] = (uintptr_t)&descs[nd++];
            }
            prev = '@';
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\n') prev = (char)c;
    }
    AEDesc res = {typeNull, NULL};
    AEBuildError berr = {0, 0};
    OSStatus st = AEBuildAppleEvent(ARG(0), ARG(1), ARG(2), ARGP(3), ARGI(4), (SInt16)ARG(5), ARGI(6), &res, &berr,
                                    fmt, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
                                    a[12], a[13], a[14], a[15], a[16], a[17], a[18], a[19], a[20], a[21], a[22],
                                    a[23]);
    if (st == noErr) aedesc_to_guest(ARG(7), &res);
    else LOGE("AEBuildAppleEvent(\"%s\") failed: %d (syntax error %u at %u)", fmt, (int)st, berr.fError, berr.fErrorPos);
    if (ARG(8)) {
        gwrite32(ARG(8), berr.fError);
        gwrite32(ARG(8) + 4, berr.fErrorPos);
    }
    GRET_ERR(st);
}

// --- event handlers ------------------------------------------------------------------------
struct ae_handler {
    AEEventClass cls;
    AEEventID id;
    gaddr_t fn, refcon;
    bool sys;
    struct ae_handler *next;
};
static struct ae_handler *handlers;
static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;

static OSErr ae_dispatch(const AppleEvent *evt, AppleEvent *reply, SRefCon refcon) {
    struct ae_handler *h = (struct ae_handler *)refcon;
    gaddr_t g = gheap_calloc(1, 2 * G_AEDESC_SIZE);
    aedesc_to_guest(g, evt);
    if (reply) aedesc_to_guest(g + G_AEDESC_SIZE, reply);
    LOGD("AE: dispatching '%.4s'/'%.4s' to guest handler %#x", (char *)&h->cls, (char *)&h->id, h->fn);
    OSErr e = (OSErr)gcall3(h->fn, g, reply ? g + G_AEDESC_SIZE : 0, h->refcon);
    if (reply) aedesc_from_guest(reply, g + G_AEDESC_SIZE);
    gheap_free(g);
    return e;
}

// AEInstallEventHandler(class, id, AEEventHandlerUPP handler, SRefCon refcon, Boolean isSysHandler)
THUNK(AEInstallEventHandler) {
    struct ae_handler *h = calloc(1, sizeof *h);
    *h = (struct ae_handler){ARG(0), ARG(1), ARG(2), ARG(3), ARGBOOL(4), NULL};
    OSErr e = AEInstallEventHandler(h->cls, h->id, ae_dispatch, (SRefCon)h, h->sys);
    if (e != noErr) {
        free(h);
        GRET_ERR(e);
        return;
    }
    pthread_mutex_lock(&handler_lock);
    // Replacing a handler for the same class/id drops the old record.
    for (struct ae_handler **pp = &handlers; *pp; pp = &(*pp)->next)
        if ((*pp)->cls == h->cls && (*pp)->id == h->id && (*pp)->sys == h->sys) {
            struct ae_handler *old = *pp;
            *pp = old->next;
            free(old);
            break;
        }
    h->next = handlers;
    handlers = h;
    pthread_mutex_unlock(&handler_lock);
    GRET_ERR(noErr);
}

// AERemoveEventHandler(class, id, handler, isSysHandler)
THUNK(AERemoveEventHandler) {
    AEEventClass cls = ARG(0);
    AEEventID id = ARG(1);
    bool sys = ARGBOOL(3);
    struct ae_handler *found = NULL;
    pthread_mutex_lock(&handler_lock);
    for (struct ae_handler **pp = &handlers; *pp; pp = &(*pp)->next)
        if ((*pp)->cls == cls && (*pp)->id == id && (*pp)->sys == sys && (!ARG(2) || (*pp)->fn == ARG(2))) {
            found = *pp;
            *pp = found->next;
            break;
        }
    pthread_mutex_unlock(&handler_lock);
    if (!found) {
        GRET_ERR(errAEHandlerNotFound);
        return;
    }
    OSErr e = AERemoveEventHandler(cls, id, ae_dispatch, sys);
    free(found);
    GRET_ERR(e);
}
