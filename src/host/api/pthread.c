// pthreads for the guest.
//
// Guest synchronisation objects (i386 layouts: mutex 44 bytes, cond 28, attr 40,
// mutexattr 12, rwlock 200, once 8) store a marker plus a pointer to a host
// object that is created on first use, so that statically initialised objects
// (PTHREAD_MUTEX_INITIALIZER etc.) work.
//
// Guest threads get a guest stack and a per-thread record with TLS slots; TLS
// keys are implemented here (values are 32-bit guest words).
#include "common.h"
#include "gthread.h"
#include "../runtime.h"

#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <fcntl.h>
#include <stdlib.h>

#define SIG_MUTEX_INIT 0x32AAABA7u
#define SIG_MUTEX_ERRORCHECK_INIT 0x32AAABA1u
#define SIG_MUTEX_RECURSIVE_INIT 0x32AAABA2u
#define SIG_COND_INIT 0x3CB0B1BBu
#define SIG_RWLOCK_INIT 0x2DA8B3B4u
#define SIG_ONCE_INIT 0x30B1BCBAu
#define SIG_MUTEX_ATTR 0x4D545841u // our own markers
#define SIG_ATTR 0x54485241u
#define DX_MUTEX 0x44584D58u
#define DX_COND 0x4458434Eu
#define DX_RWLOCK 0x4458524Cu

static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static void *load_hptr(gaddr_t g) {
    void *p;
    memcpy(&p, g2h(g), sizeof p);
    return p;
}
static void store_hptr(gaddr_t g, void *p) { memcpy(g2h(g), &p, sizeof p); }

// --- threads --------------------------------------------------------------------

#define MAX_KEYS GTHREAD_TLS_SLOTS
static struct {
    bool used;
    gaddr_t destructor;
} keys[MAX_KEYS];
static pthread_mutex_t keys_lock = PTHREAD_MUTEX_INITIALIZER;

static _Thread_local struct gthread *cur;

// Stack page -> thread block table for guest-side pthread_self/getspecific
// (libfast): entry (esp >> 14) is the handle of the thread owning that stack
// page, 0 if unknown (the guest then falls back to the host thunks).
#define STACK_TABLE_SHIFT 14
static gaddr_t stack_table;
static void make_stack_table(void) {
    stack_table = gmem_map_anywhere((1ull << 32) >> STACK_TABLE_SHIFT << 2, GPROT_RW, 0);
    if (!stack_table) fatal("pthread: no guest memory for the stack table");
}
static gaddr_t ensure_stack_table(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, make_stack_table);
    return stack_table;
}
static void init_stack_table(gaddr_t a) { gwrite32(a, ensure_stack_table()); }
GDATA(dx_stack_threads, 4, init_stack_table);

static void stack_table_set(gaddr_t lo, gaddr_t hi, gaddr_t handle) {
    if (!ensure_stack_table() || hi <= lo) return;
    for (uint64_t p = lo >> STACK_TABLE_SHIFT; p <= (uint64_t)(hi - 1) >> STACK_TABLE_SHIFT; p++)
        gwrite32(stack_table + 4 * (gaddr_t)p, handle);
}

static struct gthread *gthread_alloc(void) {
    struct gthread *t = calloc(1, sizeof *t);
    t->handle = gheap_calloc(1, GTHREAD_TLS_OFFSET + 4 * GTHREAD_TLS_SLOTS);
    gwrite32(t->handle, GTHREAD_MAGIC);
    store_hptr(t->handle + 8, t);
    t->tls = g2h(t->handle + GTHREAD_TLS_OFFSET);
    return t;
}

struct gthread *gthread_current(void) {
    if (cur) return cur;
    struct gthread *t = gthread_alloc();
    t->host = pthread_self();
    t->started = true;
    if (pthread_main_np()) {
        t->stack_lo = runtime_main_stack_lo();
        t->stack_hi = runtime_main_stack_hi();
    } else {
        cpu_thread_attach();
        cpu_get_thread_stack(&t->stack_lo, &t->stack_hi);
    }
    stack_table_set(t->stack_lo, t->stack_hi, t->handle);
    cur = t;
    return t;
}

struct gthread *gthread_from_handle(gaddr_t h) {
    if (!h || gread32(h) != GTHREAD_MAGIC) return NULL;
    return load_hptr(h + 8);
}

static void run_tls_destructors(struct gthread *t) {
    for (int iter = 0; iter < 4; iter++) {
        bool any = false;
        for (int k = 0; k < MAX_KEYS; k++) {
            uint32_t v = t->tls[k];
            if (!v || !keys[k].used || !keys[k].destructor) continue;
            t->tls[k] = 0;
            gcall1(keys[k].destructor, v);
            any = true;
        }
        if (!any) break;
    }
}

static void *thread_main(void *arg) {
    struct gthread *t = arg;
    cur = t;
    cpu_set_thread_stack(t->stack_lo, t->stack_hi);
    stack_table_set(t->stack_lo, t->stack_hi, t->handle);
    uint32_t ret = gcall1(t->start, t->arg);
    gthread_exit_current(ret);
}

_Noreturn void gthread_exit_current(uint32_t ret) {
    struct gthread *t = gthread_current();
    t->retval = ret;
    run_tls_destructors(t);
    stack_table_set(t->stack_lo, t->stack_hi, 0);
    t->finished = true;
    pthread_exit(NULL);
}

THUNK(pthread_create) {
    gaddr_t attr = ARG(1);
    uint32_t stack = 512 * 1024;
    bool detached = false;
    if (attr && gread32(attr) == SIG_ATTR) {
        if (gread32(attr + 4)) stack = gread32(attr + 4);
        detached = gread32(attr + 8) == PTHREAD_CREATE_DETACHED;
    }
    // Host code (thunks) runs on the guest stack too, so be generous.
    if (stack < 1024 * 1024) stack = 1024 * 1024;
    stack = (stack + GPAGE_SIZE - 1) & ~(GPAGE_SIZE - 1);
    struct gthread *t = gthread_alloc();
    t->stack_lo = gmem_map_anywhere(stack, GPROT_RW, 0);
    if (!t->stack_lo) RET(EAGAIN);
    t->stack_hi = t->stack_lo + stack;
    t->own_stack = true;
    t->start = ARG(2);
    t->arg = ARG(3);
    t->detached = detached;
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, 512 * 1024);
    if (detached) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->host, &ha, thread_main, t);
    pthread_attr_destroy(&ha);
    if (rc) RET(rc);
    t->started = true;
    if (ARG(0)) gwrite32(ARG(0), t->handle);
    LOGD("pthread_create: guest thread %#x start %#x stack %#x-%#x", t->handle, t->start, t->stack_lo, t->stack_hi);
    RET(0);
}

THUNK(pthread_self) { RET(gthread_current()->handle); }
// Fallbacks for libfast's guest-side versions (which shadow the names above).
THUNK(dx_pthread_self) { RET(gthread_current()->handle); }
THUNK(pthread_equal) { RET(ARG(0) == ARG(1)); }
THUNK(pthread_main_np) { RET(pthread_main_np()); }
THUNK(pthread_exit) { gthread_exit_current(ARG(0)); }

static int do_join(gaddr_t h, gaddr_t retp) {
    struct gthread *t = gthread_from_handle(h);
    if (!t) return ESRCH;
    int rc = pthread_join(t->host, NULL);
    if (rc) return rc;
    if (retp) gwrite32(retp, t->retval);
    if (t->own_stack) gmem_unmap(t->stack_lo, t->stack_hi - t->stack_lo);
    return 0;
}
THUNK(pthread_join) { RET(do_join(ARG(0), ARG(1))); }
THUNK_AS(pthread_join_unix2003, "_pthread_join$UNIX2003") { RET(do_join(ARG(0), ARG(1))); }
THUNK(pthread_detach) {
    struct gthread *t = gthread_from_handle(ARG(0));
    RET(t ? pthread_detach(t->host) : ESRCH);
}
THUNK(pthread_kill) {
    struct gthread *t = gthread_from_handle(ARG(0));
    if (!t) RET(ESRCH);
    if (ARGI(1) == 0) RET(t->finished ? ESRCH : 0);
    LOGI("pthread_kill(%#x, %d) ignored", ARG(0), ARGI(1));
    RET(0);
}
THUNK(pthread_cancel) {
    LOGI("pthread_cancel(%#x) not supported", ARG(0));
    RET(0);
}
THUNK(pthread_setname_np) {
    pthread_setname_np(ARGS(0));
    RET(0);
}
THUNK(pthread_mach_thread_np) {
    struct gthread *t = gthread_from_handle(ARG(0));
    RET(t ? pthread_mach_thread_np(t->host) : 0);
}
THUNK(pthread_get_stackaddr_np) {
    struct gthread *t = gthread_from_handle(ARG(0));
    RET(t ? t->stack_hi : 0);
}
THUNK(pthread_get_stacksize_np) {
    struct gthread *t = gthread_from_handle(ARG(0));
    RET(t ? t->stack_hi - t->stack_lo : 0);
}
// struct sched_param { int sched_priority; char opaque[4]; }
THUNK(pthread_getschedparam) {
    struct gthread *t = gthread_from_handle(ARG(0));
    if (!t) RET(ESRCH);
    int policy;
    struct sched_param sp;
    int rc = pthread_getschedparam(t->host, &policy, &sp);
    if (!rc) {
        if (ARG(1)) gwrite32(ARG(1), (uint32_t)policy);
        if (ARG(2)) gwrite32(ARG(2), (uint32_t)sp.sched_priority);
    }
    RET(rc);
}
THUNK(pthread_setschedparam) {
    struct gthread *t = gthread_from_handle(ARG(0));
    if (!t) RET(ESRCH);
    struct sched_param sp = {.sched_priority = (int)gread32(ARG(2))};
    int rc = pthread_setschedparam(t->host, ARGI(1), &sp);
    if (rc) LOGD("pthread_setschedparam failed: %d", rc);
    RET(0); // priority changes are best effort
}

// --- thread attributes (our own layout inside the 40-byte guest object) ------------
THUNK(pthread_attr_init) {
    memset(ARGP(0), 0, 40);
    gwrite32(ARG(0), SIG_ATTR);
    RET(0);
}
THUNK(pthread_attr_destroy) {
    gwrite32(ARG(0), 0);
    RET(0);
}
THUNK(pthread_attr_setstacksize) {
    gwrite32(ARG(0) + 4, ARG(1));
    RET(0);
}
THUNK(pthread_attr_getstacksize) {
    gwrite32(ARG(1), gread32(ARG(0) + 4) ? gread32(ARG(0) + 4) : 512 * 1024);
    RET(0);
}
THUNK(pthread_attr_setdetachstate) {
    gwrite32(ARG(0) + 8, ARG(1));
    RET(0);
}
THUNK(pthread_attr_getdetachstate) {
    gwrite32(ARG(1), gread32(ARG(0) + 8) ? gread32(ARG(0) + 8) : PTHREAD_CREATE_JOINABLE);
    RET(0);
}
THUNK(pthread_attr_setschedparam) { RET(0); }
THUNK(pthread_attr_getschedparam) {
    gwrite32(ARG(1), 31);
    RET(0);
}
THUNK(pthread_attr_setschedpolicy) { RET(0); }
THUNK(pthread_attr_setscope) { RET(0); }
THUNK(pthread_attr_setinheritsched) { RET(0); }

// --- TLS keys -------------------------------------------------------------------------
THUNK(pthread_key_create) {
    pthread_mutex_lock(&keys_lock);
    int k;
    for (k = 0; k < MAX_KEYS && keys[k].used; k++) {}
    if (k == MAX_KEYS) {
        pthread_mutex_unlock(&keys_lock);
        RET(EAGAIN);
    }
    keys[k].used = true;
    keys[k].destructor = ARG(1);
    pthread_mutex_unlock(&keys_lock);
    gwrite32(ARG(0), (uint32_t)k);
    RET(0);
}
THUNK(pthread_key_delete) {
    uint32_t k = ARG(0);
    if (k >= MAX_KEYS) RET(EINVAL);
    keys[k].used = false;
    keys[k].destructor = 0;
    RET(0);
}
THUNK(pthread_getspecific) {
    uint32_t k = ARG(0);
    RET(k < MAX_KEYS ? gthread_current()->tls[k] : 0);
}
THUNK(pthread_setspecific) {
    uint32_t k = ARG(0);
    if (k >= MAX_KEYS) RET(EINVAL);
    gthread_current()->tls[k] = ARG(1);
    RET(0);
}

// --- once --------------------------------------------------------------------------
// pthread_once_t { long sig; char opaque[4]; } -> opaque holds 0 (new), 1 (running), 2 (done)
static pthread_mutex_t once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
THUNK(pthread_once) {
    gaddr_t o = ARG(0);
    if (__atomic_load_n((uint32_t *)g2h(o + 4), __ATOMIC_ACQUIRE) == 2) RET(0);
    pthread_mutex_lock(&once_lock);
    if (gread32(o + 4) == 0) {
        gwrite32(o + 4, 1);
        gcall0(ARG(1));
        __atomic_store_n((uint32_t *)g2h(o + 4), 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    RET(0);
}

// --- mutexes ---------------------------------------------------------------------------
static pthread_mutex_t *create_mutex(int type) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type);
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

static pthread_mutex_t *gmutex(gaddr_t g) {
    uint32_t *sig = g2h(g);
    if (__atomic_load_n(sig, __ATOMIC_ACQUIRE) == DX_MUTEX) return load_hptr(g + 4);
    pthread_mutex_lock(&lazy_lock);
    if (*sig != DX_MUTEX) {
        int type = PTHREAD_MUTEX_NORMAL;
        if (*sig == SIG_MUTEX_RECURSIVE_INIT) type = PTHREAD_MUTEX_RECURSIVE;
        else if (*sig == SIG_MUTEX_ERRORCHECK_INIT) type = PTHREAD_MUTEX_ERRORCHECK;
        else if (*sig != SIG_MUTEX_INIT) LOGD("pthread: mutex %#x used without init (sig %#x)", g, *sig);
        store_hptr(g + 4, create_mutex(type));
        __atomic_store_n(sig, DX_MUTEX, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lazy_lock);
    return load_hptr(g + 4);
}

THUNK(pthread_mutexattr_init) {
    gwrite32(ARG(0), SIG_MUTEX_ATTR);
    gwrite32(ARG(0) + 4, PTHREAD_MUTEX_DEFAULT);
    RET(0);
}
THUNK(pthread_mutexattr_destroy) { RET(0); }
THUNK(pthread_mutexattr_settype) {
    gwrite32(ARG(0) + 4, ARG(1));
    RET(0);
}
THUNK(pthread_mutexattr_gettype) {
    gwrite32(ARG(1), gread32(ARG(0) + 4));
    RET(0);
}
THUNK(pthread_mutexattr_setpshared) { RET(0); }
THUNK(pthread_mutexattr_setprotocol) { RET(0); }

THUNK(pthread_mutex_init) {
    int type = PTHREAD_MUTEX_DEFAULT;
    if (ARG(1) && gread32(ARG(1)) == SIG_MUTEX_ATTR) type = (int)gread32(ARG(1) + 4);
    store_hptr(ARG(0) + 4, create_mutex(type));
    __atomic_store_n((uint32_t *)ARGP(0), DX_MUTEX, __ATOMIC_RELEASE);
    RET(0);
}
THUNK(pthread_mutex_destroy) {
    if (gread32(ARG(0)) == DX_MUTEX) {
        pthread_mutex_t *m = load_hptr(ARG(0) + 4);
        pthread_mutex_destroy(m);
        free(m);
    }
    gwrite32(ARG(0), 0);
    RET(0);
}
THUNK(pthread_mutex_lock) { RET(pthread_mutex_lock(gmutex(ARG(0)))); }
THUNK(pthread_mutex_trylock) { RET(pthread_mutex_trylock(gmutex(ARG(0)))); }
THUNK(pthread_mutex_unlock) { RET(pthread_mutex_unlock(gmutex(ARG(0)))); }

// --- condition variables -----------------------------------------------------------------
static pthread_cond_t *gcond(gaddr_t g) {
    uint32_t *sig = g2h(g);
    if (__atomic_load_n(sig, __ATOMIC_ACQUIRE) == DX_COND) return load_hptr(g + 4);
    pthread_mutex_lock(&lazy_lock);
    if (*sig != DX_COND) {
        pthread_cond_t *c = malloc(sizeof *c);
        pthread_cond_init(c, NULL);
        store_hptr(g + 4, c);
        __atomic_store_n(sig, DX_COND, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lazy_lock);
    return load_hptr(g + 4);
}
static int cond_init(gaddr_t g) {
    pthread_cond_t *c = malloc(sizeof *c);
    pthread_cond_init(c, NULL);
    store_hptr(g + 4, c);
    __atomic_store_n((uint32_t *)g2h(g), DX_COND, __ATOMIC_RELEASE);
    return 0;
}
THUNK(pthread_cond_init) { RET(cond_init(ARG(0))); }
THUNK_AS(pthread_cond_init_unix2003, "_pthread_cond_init$UNIX2003") { RET(cond_init(ARG(0))); }
THUNK(pthread_cond_destroy) {
    if (gread32(ARG(0)) == DX_COND) {
        pthread_cond_t *c = load_hptr(ARG(0) + 4);
        pthread_cond_destroy(c);
        free(c);
    }
    gwrite32(ARG(0), 0);
    RET(0);
}
THUNK(pthread_cond_signal) { RET(pthread_cond_signal(gcond(ARG(0)))); }
THUNK(pthread_cond_broadcast) { RET(pthread_cond_broadcast(gcond(ARG(0)))); }
THUNK(pthread_cond_wait) { RET(pthread_cond_wait(gcond(ARG(0)), gmutex(ARG(1)))); }
THUNK_AS(pthread_cond_wait_unix2003, "_pthread_cond_wait$UNIX2003") {
    RET(pthread_cond_wait(gcond(ARG(0)), gmutex(ARG(1))));
}
static int cond_timedwait(gaddr_t c, gaddr_t m, gaddr_t ts) {
    struct timespec t;
    get_gtimespec(ts, &t);
    return pthread_cond_timedwait(gcond(c), gmutex(m), &t);
}
THUNK(pthread_cond_timedwait) { RET(cond_timedwait(ARG(0), ARG(1), ARG(2))); }
THUNK_AS(pthread_cond_timedwait_unix2003, "_pthread_cond_timedwait$UNIX2003") {
    RET(cond_timedwait(ARG(0), ARG(1), ARG(2)));
}
THUNK(pthread_cond_timedwait_relative_np) {
    struct timespec t;
    get_gtimespec(ARG(2), &t);
    RET(pthread_cond_timedwait_relative_np(gcond(ARG(0)), gmutex(ARG(1)), &t));
}

// --- rwlocks ------------------------------------------------------------------------------
static pthread_rwlock_t *grwlock(gaddr_t g) {
    uint32_t *sig = g2h(g);
    if (__atomic_load_n(sig, __ATOMIC_ACQUIRE) == DX_RWLOCK) return load_hptr(g + 4);
    pthread_mutex_lock(&lazy_lock);
    if (*sig != DX_RWLOCK) {
        pthread_rwlock_t *l = malloc(sizeof *l);
        pthread_rwlock_init(l, NULL);
        store_hptr(g + 4, l);
        __atomic_store_n(sig, DX_RWLOCK, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lazy_lock);
    return load_hptr(g + 4);
}
#define RWLOCK_THUNKS(suffix, name)                                                                \
    THUNK_AS(rw_init##suffix, "_pthread_rwlock_init" name) {                                       \
        gwrite32(ARG(0), SIG_RWLOCK_INIT);                                                         \
        grwlock(ARG(0));                                                                           \
        RET(0);                                                                                    \
    }                                                                                              \
    THUNK_AS(rw_destroy##suffix, "_pthread_rwlock_destroy" name) {                                 \
        if (gread32(ARG(0)) == DX_RWLOCK) {                                                        \
            pthread_rwlock_t *l = load_hptr(ARG(0) + 4);                                           \
            pthread_rwlock_destroy(l);                                                             \
            free(l);                                                                               \
        }                                                                                          \
        gwrite32(ARG(0), 0);                                                                       \
        RET(0);                                                                                    \
    }                                                                                              \
    THUNK_AS(rw_rd##suffix, "_pthread_rwlock_rdlock" name) { RET(pthread_rwlock_rdlock(grwlock(ARG(0)))); } \
    THUNK_AS(rw_wr##suffix, "_pthread_rwlock_wrlock" name) { RET(pthread_rwlock_wrlock(grwlock(ARG(0)))); } \
    THUNK_AS(rw_tryrd##suffix, "_pthread_rwlock_tryrdlock" name) { RET(pthread_rwlock_tryrdlock(grwlock(ARG(0)))); } \
    THUNK_AS(rw_trywr##suffix, "_pthread_rwlock_trywrlock" name) { RET(pthread_rwlock_trywrlock(grwlock(ARG(0)))); } \
    THUNK_AS(rw_unlock##suffix, "_pthread_rwlock_unlock" name) { RET(pthread_rwlock_unlock(grwlock(ARG(0)))); }
RWLOCK_THUNKS(, "")
RWLOCK_THUNKS(_unix2003, "$UNIX2003")

// --- POSIX semaphores (FMOD uses named semaphores) ---------------------------------------------
THUNK(sem_open) {
    sem_t *s = ARGI(1) & O_CREAT ? sem_open(ARGS(0), ARGI(1), (mode_t)ARG(2), ARG(3)) : sem_open(ARGS(0), ARGI(1));
    if (s == SEM_FAILED) {
        set_gerrno(errno);
        RET(0xFFFFFFFFu);
    }
    RET(hobj_wrap(s));
}
THUNK(sem_close) { RET_ERRNO(sem_close(hobj_unwrap(ARG(0)))); }
THUNK(sem_unlink) { RET_ERRNO(sem_unlink(ARGS(0))); }
THUNK(sem_wait) { RET_ERRNO(sem_wait(hobj_unwrap(ARG(0)))); }
THUNK(sem_trywait) { RET_ERRNO(sem_trywait(hobj_unwrap(ARG(0)))); }
THUNK(sem_post) { RET_ERRNO(sem_post(hobj_unwrap(ARG(0)))); }

// --- Mach thread calls -------------------------------------------------------------------------
THUNK(mach_thread_self) { RET(mach_thread_self()); }
THUNK(thread_suspend) { RET(thread_suspend(ARG(0))); }
THUNK(thread_resume) { RET(thread_resume(ARG(0))); }
THUNK(thread_info) {
    // thread_info(port, flavor, info_out, count_inout): natural_t arrays, same size on both sides.
    mach_msg_type_number_t cnt = gread32(ARG(3));
    kern_return_t kr = thread_info(ARG(0), ARG(1), (thread_info_t)ARGP(2), &cnt);
    gwrite32(ARG(3), cnt);
    RET(kr);
}
THUNK(thread_get_state) {
    // Guest register state of other threads is not available through the host kernel.
    RET(KERN_FAILURE);
}
THUNK(task_threads) {
    thread_act_array_t list;
    mach_msg_type_number_t n;
    kern_return_t kr = task_threads(mach_task_self(), &list, &n);
    if (kr != KERN_SUCCESS) RET(kr);
    gaddr_t g = gheap_alloc(4 * n);
    for (mach_msg_type_number_t i = 0; i < n; i++) gwrite32(g + 4 * i, list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
    gwrite32(ARG(1), g);
    gwrite32(ARG(2), n);
    RET(KERN_SUCCESS);
}
