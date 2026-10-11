// CoreServices (CarbonCore) legacy utilities implemented on guest memory:
// Memory Manager pointers, OS queues, bit operations, fixed-point math,
// Notification Manager, Open Transport atomics, A5 world.
#include "common.h"

#include <pthread.h>
#include <stdatomic.h>

enum { noErr = 0, memFullErr = -108, qErr = -1, nilHandleErr = -109 };

// --- Memory Manager ---------------------------------------------------------------
static _Thread_local int16_t mem_error;

static gaddr_t new_ptr(uint32_t size, bool clear) {
    if ((int32_t)size < 0) {
        mem_error = memFullErr;
        return 0;
    }
    gaddr_t p = clear ? gheap_calloc(1, size ? size : 1) : gheap_alloc(size ? size : 1);
    mem_error = p ? noErr : memFullErr;
    return p;
}

THUNK(NewPtr) { RET(new_ptr(ARG(0), false)); }
THUNK(NewPtrClear) { RET(new_ptr(ARG(0), true)); }
THUNK(NewPtrSys) { RET(new_ptr(ARG(0), false)); }
THUNK(NewPtrSysClear) { RET(new_ptr(ARG(0), true)); }
THUNK(DisposePtr) {
    gheap_free(ARG(0));
    mem_error = noErr;
}
THUNK(GetPtrSize) {
    mem_error = noErr;
    RET(gheap_size(ARG(0)));
}
THUNK(MemError) { RET((uint32_t)(int32_t)mem_error); }
// Memory is never really short; report generous values (32-bit Size).
THUNK(CompactMem) { RET(0x40000000u); }
THUNK(FreeMem) { RET(0x40000000u); }
THUNK(MaxBlock) { RET(0x40000000u); }

// --- OS queues (QHdr/QElem, i386 layouts) -------------------------------------------
// QHdr  { SInt16 qFlags; QElemPtr qHead @2; QElemPtr qTail @6 }   (10 bytes, pack 2)
// QElem { QElemPtr qLink @0; SInt16 qType @4; SInt16 qData[1] @6 }
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

THUNK(Enqueue) {
    gaddr_t el = ARG(0), q = ARG(1);
    pthread_mutex_lock(&queue_lock);
    gwrite32(el, 0);
    gaddr_t tail = gread32(q + 6);
    if (tail) gwrite32(tail, el);
    else gwrite32(q + 2, el);
    gwrite32(q + 6, el);
    pthread_mutex_unlock(&queue_lock);
}

THUNK(Dequeue) {
    gaddr_t el = ARG(0), q = ARG(1);
    int16_t err = qErr;
    pthread_mutex_lock(&queue_lock);
    gaddr_t prev = 0, cur = gread32(q + 2);
    while (cur) {
        if (cur == el) {
            gaddr_t next = gread32(cur);
            if (prev) gwrite32(prev, next);
            else gwrite32(q + 2, next);
            if (gread32(q + 6) == cur) gwrite32(q + 6, prev);
            err = noErr;
            break;
        }
        prev = cur;
        cur = gread32(cur);
    }
    pthread_mutex_unlock(&queue_lock);
    RET((uint32_t)(int32_t)err);
}

// --- bit operations (ToolUtils: bit 0 is the MSB of the first byte) -------------------
THUNK(BitTst) {
    int32_t n = ARGI(1);
    RET((gread8(ARG(0) + (uint32_t)(n >> 3)) & (0x80 >> (n & 7))) ? 1 : 0);
}
THUNK(BitSet) {
    int32_t n = ARGI(1);
    gaddr_t a = ARG(0) + (uint32_t)(n >> 3);
    gwrite8(a, gread8(a) | (uint8_t)(0x80 >> (n & 7)));
}
THUNK(BitClr) {
    int32_t n = ARGI(1);
    gaddr_t a = ARG(0) + (uint32_t)(n >> 3);
    gwrite8(a, gread8(a) & (uint8_t) ~(0x80 >> (n & 7)));
}

// Open Transport: bit numbers count from the LSB; returns the previous value.
THUNK(OTAtomicSetBit) {
    uint8_t mask = (uint8_t)(1u << (ARG(1) & 7));
    uint8_t old = __atomic_fetch_or((uint8_t *)g2h(ARG(0)), mask, __ATOMIC_SEQ_CST);
    RET((old & mask) ? 1 : 0);
}
THUNK(OTAtomicClearBit) {
    uint8_t mask = (uint8_t)(1u << (ARG(1) & 7));
    uint8_t old = __atomic_fetch_and((uint8_t *)g2h(ARG(0)), (uint8_t)~mask, __ATOMIC_SEQ_CST);
    RET((old & mask) ? 1 : 0);
}
THUNK(OTAtomicTestBit) { RET((gread8(ARG(0)) >> (ARG(1) & 7)) & 1); }

// 68k A5 world: meaningless on Intel.
THUNK(SetCurrentA5) { RET(0); }
THUNK(SetA5) { RET(0); }

// --- fixed point ----------------------------------------------------------------------
THUNK(Fix2Long) {
    int32_t x = ARGI(0);
    // Rounds to nearest (FixMath.h semantics).
    RET((uint32_t)(int32_t)(((int64_t)x + 0x8000) >> 16));
}
THUNK(Long2Fix) {
    int64_t v = (int64_t)ARGI(0) << 16;
    if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
    if (v < -0x7FFFFFFF - 1) v = -0x7FFFFFFF - 1;
    RET((uint32_t)(int32_t)v);
}
THUNK(Fix2X) { RETD((double)ARGI(0) / 65536.0); }
THUNK(X2Fix) {
    double d = ARGD(0) * 65536.0;
    int32_t v;
    if (d >= 2147483647.0) v = 0x7FFFFFFF;
    else if (d <= -2147483648.0) v = (int32_t)0x80000000;
    else v = (int32_t)(d < 0 ? d - 0.5 : d + 0.5);
    RET((uint32_t)v);
}
THUNK(FixMul) { RET((uint32_t)(int32_t)(((int64_t)ARGI(0) * ARGI(1) + 0x8000) >> 16)); }
THUNK(FixRatio) {
    int16_t n = (int16_t)ARG(0), d = (int16_t)ARG(1);
    RET(d ? (uint32_t)(int32_t)(((int64_t)n << 16) / d) : (n < 0 ? 0x80000000u : 0x7FFFFFFFu));
}

// --- Notification Manager --------------------------------------------------------------
// NMRec (i386, pack 2): qLink @0, qType @4, nmFlags @6, nmPrivate @8, nmReserved @12,
// nmMark @14, nmIcon @16, nmSound @20, nmStr @24 (StringPtr), nmResp @28, nmRefCon @32.
THUNK(NMInstall) {
    gaddr_t rec = ARG(0);
    gaddr_t str = rec ? gread32(rec + 24) : 0;
    if (str) {
        uint8_t n = gread8(str);
        LOGI("NMInstall: \"%.*s\"", n, (const char *)g2h(str + 1));
    } else
        LOGI("NMInstall (no text)");
    RET(noErr);
}
THUNK(NMRemove) { RET(noErr); }
