// Assorted libSystem pieces: string interning helper, OSAtomic / OSSpinLock,
// keymgr, __assert_rtn, POSIX regex, sched_*.
#include "common.h"
#include "libc.h"
#include "../strmap.h"

#include <pthread.h>
#include <regex.h>
#include <sched.h>
#include <stdlib.h>

// --- interned guest strings ---------------------------------------------------------
static struct strmap intern_map;
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;

gaddr_t gintern(const char *s) {
    if (!s) return 0;
    pthread_mutex_lock(&intern_lock);
    if (!intern_map.slots) strmap_init(&intern_map);
    uint64_t v;
    if (!strmap_get(&intern_map, s, &v)) {
        v = gheap_strdup(s);
        strmap_put(&intern_map, s, v, false);
    }
    pthread_mutex_unlock(&intern_lock);
    return (gaddr_t)v;
}

// --- OSAtomic (guest memory is accessed atomically in place) ----------------------------
#define A32(i) ((int32_t *)g2h(ARG(i)))
#define A64(i) ((int64_t *)g2h(ARG(i)))
#define SEQ __ATOMIC_SEQ_CST

THUNK(OSMemoryBarrier) { __atomic_thread_fence(SEQ); }

#define ATOMIC_ADD32(name)                                                                         \
    THUNK(name) { RET(__atomic_add_fetch(A32(1), ARGI(0), SEQ)); }
ATOMIC_ADD32(OSAtomicAdd32)
ATOMIC_ADD32(OSAtomicAdd32Barrier)
THUNK(OSAtomicIncrement32) { RET(__atomic_add_fetch(A32(0), 1, SEQ)); }
THUNK(OSAtomicIncrement32Barrier) { RET(__atomic_add_fetch(A32(0), 1, SEQ)); }
THUNK(OSAtomicDecrement32) { RET(__atomic_sub_fetch(A32(0), 1, SEQ)); }
THUNK(OSAtomicDecrement32Barrier) { RET(__atomic_sub_fetch(A32(0), 1, SEQ)); }

THUNK(OSAtomicAdd64) { RET64(__atomic_add_fetch(A64(2), (int64_t)ARG64(0), SEQ)); }
THUNK(OSAtomicAdd64Barrier) { RET64(__atomic_add_fetch(A64(2), (int64_t)ARG64(0), SEQ)); }
THUNK(OSAtomicIncrement64) { RET64(__atomic_add_fetch(A64(0), 1, SEQ)); }
THUNK(OSAtomicIncrement64Barrier) { RET64(__atomic_add_fetch(A64(0), 1, SEQ)); }
THUNK(OSAtomicDecrement64) { RET64(__atomic_sub_fetch(A64(0), 1, SEQ)); }
THUNK(OSAtomicDecrement64Barrier) { RET64(__atomic_sub_fetch(A64(0), 1, SEQ)); }

// Bitwise ops: (uint32_t mask, volatile uint32_t *value); plain forms return the new value.
#define ATOMIC_BITOP(op)                                                                           \
    THUNK(OSAtomic##op##32) { RET(__atomic_##op##_fetch_n(A32(1), ARGI(0))); }                    \
    THUNK(OSAtomic##op##32Barrier) { RET(__atomic_##op##_fetch_n(A32(1), ARGI(0))); }             \
    THUNK(OSAtomic##op##32Orig) { RET(__atomic_fetch_##op##_n(A32(1), ARGI(0))); }                \
    THUNK(OSAtomic##op##32OrigBarrier) { RET(__atomic_fetch_##op##_n(A32(1), ARGI(0))); }
#define __atomic_Or_fetch_n(p, v) __atomic_or_fetch(p, v, SEQ)
#define __atomic_And_fetch_n(p, v) __atomic_and_fetch(p, v, SEQ)
#define __atomic_Xor_fetch_n(p, v) __atomic_xor_fetch(p, v, SEQ)
#define __atomic_fetch_Or_n(p, v) __atomic_fetch_or(p, v, SEQ)
#define __atomic_fetch_And_n(p, v) __atomic_fetch_and(p, v, SEQ)
#define __atomic_fetch_Xor_n(p, v) __atomic_fetch_xor(p, v, SEQ)
ATOMIC_BITOP(Or)
ATOMIC_BITOP(And)
ATOMIC_BITOP(Xor)

// Compare-and-swap: (old, new, ptr) -> bool. int/long/pointer are all 32-bit in the guest.
static bool cas32(gaddr_t p, uint32_t old, uint32_t nv) {
    return __atomic_compare_exchange_n((uint32_t *)g2h(p), &old, nv, false, SEQ, SEQ);
}
#define CAS32(name) THUNK(name) { RET(cas32(ARG(2), ARG(0), ARG(1)) ? 1 : 0); }
CAS32(OSAtomicCompareAndSwap32)
CAS32(OSAtomicCompareAndSwap32Barrier)
CAS32(OSAtomicCompareAndSwapInt)
CAS32(OSAtomicCompareAndSwapIntBarrier)
CAS32(OSAtomicCompareAndSwapLong)
CAS32(OSAtomicCompareAndSwapLongBarrier)
CAS32(OSAtomicCompareAndSwapPtr)
CAS32(OSAtomicCompareAndSwapPtrBarrier)
static bool cas64(gaddr_t p, uint64_t old, uint64_t nv) {
    return __atomic_compare_exchange_n((uint64_t *)g2h(p), &old, nv, false, SEQ, SEQ);
}
THUNK(OSAtomicCompareAndSwap64) { RET(cas64(ARG(4), ARG64(0), ARG64(2)) ? 1 : 0); }
THUNK(OSAtomicCompareAndSwap64Barrier) { RET(cas64(ARG(4), ARG64(0), ARG64(2)) ? 1 : 0); }

// Test-and-set: bit n is (0x80 >> (n & 7)) of byte addr + (n >> 3). Returns the old bit.
static int test_and(gaddr_t addr, uint32_t n, bool set) {
    uint8_t *b = (uint8_t *)g2h(addr + (n >> 3));
    uint8_t mask = (uint8_t)(0x80u >> (n & 7));
    uint8_t old = set ? __atomic_fetch_or(b, mask, SEQ) : __atomic_fetch_and(b, (uint8_t)~mask, SEQ);
    return (old & mask) != 0;
}
THUNK(OSAtomicTestAndSet) { RET(test_and(ARG(1), ARG(0), true)); }
THUNK(OSAtomicTestAndSetBarrier) { RET(test_and(ARG(1), ARG(0), true)); }
THUNK(OSAtomicTestAndClear) { RET(test_and(ARG(1), ARG(0), false)); }
THUNK(OSAtomicTestAndClearBarrier) { RET(test_and(ARG(1), ARG(0), false)); }

// OSSpinLock: a 32-bit int in guest memory, 0 = unlocked.
static bool spin_try(gaddr_t p) {
    uint32_t zero = 0;
    return __atomic_compare_exchange_n((uint32_t *)g2h(p), &zero, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
THUNK(OSSpinLockTry) { RET(spin_try(ARG(0)) ? 1 : 0); }
THUNK(OSSpinLockLock) {
    for (int spins = 0; !spin_try(ARG(0)); spins++) {
        if (spins < 64) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        }
        else sched_yield();
    }
}
THUNK(OSSpinLockUnlock) { __atomic_store_n((uint32_t *)g2h(ARG(0)), 0, __ATOMIC_RELEASE); }
// Older names for the same thing.
THUNK_ALIAS(spin_lock, "__spin_lock", OSSpinLockLock)
THUNK_ALIAS(spin_unlock, "__spin_unlock", OSSpinLockUnlock)
THUNK_ALIAS(spin_lock_try, "__spin_lock_try", OSSpinLockTry)

// OSQueueHead {void *opaque1; long opaque2}: a LIFO linked through 'offset'.
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
THUNK(OSAtomicEnqueue) {
    gaddr_t head = ARG(0), item = ARG(1), off = ARG(2);
    pthread_mutex_lock(&queue_lock);
    gwrite32(item + off, gread32(head));
    gwrite32(head, item);
    gwrite32(head + 4, gread32(head + 4) + 1);
    pthread_mutex_unlock(&queue_lock);
}
THUNK(OSAtomicDequeue) {
    gaddr_t head = ARG(0), off = ARG(1);
    pthread_mutex_lock(&queue_lock);
    gaddr_t item = gread32(head);
    if (item) {
        gwrite32(head, gread32(item + off));
        gwrite32(head + 4, gread32(head + 4) + 1);
    }
    pthread_mutex_unlock(&queue_lock);
    RET(item);
}

// --- keymgr (used by the libgcc/FMOD EH runtime) ----------------------------------------------
#define NKEYS 64
static struct {
    uint32_t key;
    gaddr_t value;
    pthread_mutex_t lock;
    bool used;
} keys[NKEYS];
static pthread_mutex_t keys_lock = PTHREAD_MUTEX_INITIALIZER;

static int key_slot(uint32_t key) {
    pthread_mutex_lock(&keys_lock);
    int i, freeslot = -1;
    for (i = 0; i < NKEYS; i++) {
        if (keys[i].used && keys[i].key == key) break;
        if (!keys[i].used && freeslot < 0) freeslot = i;
    }
    if (i == NKEYS) {
        if (freeslot < 0) fatal("keymgr: too many keys");
        i = freeslot;
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&keys[i].lock, &a);
        pthread_mutexattr_destroy(&a);
        keys[i].key = key;
        keys[i].value = 0;
        keys[i].used = true;
    }
    pthread_mutex_unlock(&keys_lock);
    return i;
}

THUNK(_keymgr_get_and_lock_processwide_ptr) {
    int i = key_slot(ARG(0));
    pthread_mutex_lock(&keys[i].lock);
    RET(keys[i].value);
}
THUNK(_keymgr_get_and_lock_processwide_ptr_2) {
    int i = key_slot(ARG(0));
    pthread_mutex_lock(&keys[i].lock);
    if (ARG(1)) gwrite32(ARG(1), keys[i].value);
    RET(0);
}
THUNK(_keymgr_set_and_unlock_processwide_ptr) {
    int i = key_slot(ARG(0));
    keys[i].value = ARG(1);
    RET(pthread_mutex_unlock(&keys[i].lock) ? EPERM : 0);
}
THUNK(_keymgr_unlock_processwide_ptr) {
    int i = key_slot(ARG(0));
    RET(pthread_mutex_unlock(&keys[i].lock) ? EPERM : 0);
}
THUNK(_keymgr_get_per_thread_data) { RET(0); }

// --- assertions -------------------------------------------------------------------------------
// __assert_rtn(const char *func, const char *file, int line, const char *expr)
THUNK(__assert_rtn) {
    fatal("guest assertion failed: (%s), function %s, file %s, line %d", ARG(3) ? ARGS(3) : "?",
          ARG(0) ? ARGS(0) : "?", ARG(1) ? ARGS(1) : "?", ARGI(2));
}
THUNK(__assert) {
    fatal("guest assertion failed: (%s), file %s, line %d", ARG(0) ? ARGS(0) : "?", ARG(1) ? ARGS(1) : "?",
          ARGI(2));
}

// --- POSIX regex ------------------------------------------------------------------------------
// i386 regex_t: re_magic@0, re_nsub@4, re_endp@8, re_g@12 (16 bytes). The host
// regex_t is kept behind a handle in re_g. regmatch_t (two off_t) is identical.
static regex_t *greg(gaddr_t preg) { return preg ? hobj_unwrap(gread32(preg + 12)) : NULL; }

THUNK(regcomp) {
    gaddr_t preg = ARG(0);
    int cflags = ARGI(2);
    regex_t *h = calloc(1, sizeof *h);
    if (cflags & REG_PEND) h->re_endp = g2h(gread32(preg + 8));
    int rc = regcomp(h, ARGS(1), cflags);
    if (rc) {
        free(h);
        RET(rc);
        return;
    }
    gwrite32(preg, (uint32_t)h->re_magic);
    gwrite32(preg + 4, (uint32_t)h->re_nsub);
    gwrite32(preg + 8, 0);
    gwrite32(preg + 12, hobj_wrap(h));
    RET(0);
}
THUNK_ALIAS(regcomp_unix2003, "_regcomp$UNIX2003", regcomp)
THUNK(regexec) {
    regex_t *h = greg(ARG(0));
    if (!h) {
        RET(REG_INVARG);
        return;
    }
    RET(regexec(h, ARGS(1), ARG(2), ARGP(3), ARGI(4)));
}
THUNK(regerror) { RET(regerror(ARGI(0), greg(ARG(1)), ARGP(2), ARG(3))); }
THUNK(regfree) {
    regex_t *h = greg(ARG(0));
    if (!h) return;
    regfree(h);
    hobj_forget(gread32(ARG(0) + 12));
    free(h);
    gwrite32(ARG(0) + 12, 0);
}

// --- scheduling ---------------------------------------------------------------------------------
THUNK(sched_yield) { RET(sched_yield()); }
THUNK(sched_get_priority_min) { RET_ERRNO(sched_get_priority_min(ARGI(0))); }
THUNK(sched_get_priority_max) { RET_ERRNO(sched_get_priority_max(ARGI(0))); }
