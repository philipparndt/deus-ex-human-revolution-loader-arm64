// CPU backend: native arm64 host, guest i386 code executed by dxcpu (our Rust
// interpreter/JIT in src/cpu). Same structure as the Unicorn backend: a
// reserved stub range for thunks, one engine per host->guest nesting depth.
#include "cpu.h"
#include "dxcpu.h"
#include "log.h"
#include "macho.h"
#include "thunk.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#define STUB_BASE 0xFFC00000u
#define MAX_STUBS 65536u
#define STUB_SIZE 16u
#define RET_BASE (STUB_BASE + MAX_STUBS * STUB_SIZE)
#define MAX_DEPTH 32
#define THREAD_GUEST_STACK (1u << 20)

struct dthread {
    dxcpu_engine *eng[MAX_DEPTH];
    dxcpu_state *st[MAX_DEPTH];
    int depth;
    bool in_thunk;
    uint32_t esp_hint;
    gaddr_t stack_lo, stack_hi;
    // watchdog: the host call this thread is in (guest registers at the call)
    struct guest_regs *volatile call;
    pthread_t self;
    bool is_main;
    struct dthread *next;
};

static _Thread_local struct dthread *dt;
static struct dthread *all_threads;
static pthread_mutex_t all_lock = PTHREAD_MUTEX_INITIALIZER;

static struct dthread *cur(void) {
    if (!dt) {
        dt = calloc(1, sizeof *dt);
        dt->self = pthread_self();
        dt->is_main = pthread_main_np();
        pthread_mutex_lock(&all_lock);
        dt->next = all_threads;
        all_threads = dt;
        pthread_mutex_unlock(&all_lock);
    }
    return dt;
}

// --- watchdog ------------------------------------------------------------------------------------
// DXH_WATCHDOG=<seconds>: when the main thread has made no host call and run no
// event loop iteration for that long (typically blocked inside a nested guest
// call), log every guest thread's current host call and guest frames once.
static volatile uint64_t main_beat;
static int watchdog_secs;

void cpu_main_heartbeat(void) {
    if (watchdog_secs) main_beat = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void dump_guest_frames(gaddr_t esp, gaddr_t ebp, const char *indent) {
    char where[256];
    gaddr_t ret = gread32(esp);
    LOGE("%sreturns to %#x %s", indent, ret, macho_describe_addr(ret, where, sizeof where));
    for (int i = 0; i < 16 && ebp >= esp && ebp - esp < (1u << 24); i++) {
        gaddr_t ra = gread32(ebp + 4);
        LOGE("%s#%-2d %#x %s", indent, i, ra, macho_describe_addr(ra, where, sizeof where));
        gaddr_t next = gread32(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
}

static void *watchdog_thread(void *arg) {
    bool dumped = false;
    for (;;) {
        sleep(1);
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), beat = main_beat;
        if (!beat) continue;
        if (now - beat < (uint64_t)watchdog_secs * 1000000000ull) {
            dumped = false;
            continue;
        }
        if (dumped) continue;
        dumped = true;
        LOGE("watchdog: main thread silent for %.1f s, guest threads:", (now - beat) / 1e9);
        pthread_mutex_lock(&all_lock);
        for (struct dthread *t = all_threads; t; t = t->next) {
            char name[64] = "";
            if (t->is_main) strlcpy(name, "main", sizeof name);
            else pthread_getname_np(t->self, name, sizeof name);
            struct guest_regs *r = t->call;
            if (r && t->in_thunk) {
                LOGE("  thread %s (depth %d) in %s, esp %#x ebp %#x", name[0] ? name : "?", t->depth,
                     thunk_name(r->thunk_id), r->esp, r->ebp);
                dump_guest_frames(r->esp, r->ebp, "      ");
            } else {
                LOGE("  thread %s (depth %d) running guest code", name[0] ? name : "?", t->depth);
            }
        }
        pthread_mutex_unlock(&all_lock);
    }
    return arg;
}

static void ensure_depth(int depth) {
    struct dthread *t = cur();
    if (depth >= MAX_DEPTH) fatal("dxcpu: host/guest call nesting too deep");
    if (!t->eng[depth]) {
        t->eng[depth] = dxcpu_engine_new(STUB_BASE, RET_BASE + MAX_DEPTH * STUB_SIZE);
        t->st[depth] = dxcpu_state_new();
    }
}

// DXCPU_STATS=<seconds>: print and reset the JIT counters periodically.
static void *stats_thread(void *arg) {
    unsigned secs = (unsigned)(uintptr_t)arg;
    for (;;) {
        sleep(secs);
        dxcpu_print_stats();
    }
    return NULL;
}

void cpu_init(void) {
    const char *wd = getenv("DXH_WATCHDOG");
    if (wd && atoi(wd) > 0) {
        watchdog_secs = atoi(wd);
        pthread_t w;
        pthread_create(&w, NULL, watchdog_thread, NULL);
        pthread_detach(w);
    }
    const char *stats = getenv("DXCPU_STATS");
    if (stats && atoi(stats) > 0) {
        pthread_t t;
        pthread_create(&t, NULL, stats_thread, (void *)(uintptr_t)atoi(stats));
        pthread_detach(t);
    }
    if (!guest_base) fatal("dxcpu backend requires a guest base");
    dxcpu_set_guest_base(guest_base);
    dxcpu_set_mapped_query(gmem_is_mapped);
    if (!gmem_map_fixed(STUB_BASE, MAX_STUBS * STUB_SIZE + MAX_DEPTH * STUB_SIZE, GPROT_RW))
        fatal("dxcpu: cannot reserve stub range");
    memset(g2h(STUB_BASE), 0xF4, MAX_STUBS * STUB_SIZE + MAX_DEPTH * STUB_SIZE);
    LOGI("dxcpu: backend ready, stubs at %#x", STUB_BASE);
}

gaddr_t cpu_make_stub(uint32_t id) {
    if (id >= MAX_STUBS) fatal("dxcpu: too many thunk stubs");
    return STUB_BASE + id * STUB_SIZE;
}

void cpu_set_thread_stack(gaddr_t lo, gaddr_t hi) {
    cur()->stack_lo = lo;
    cur()->stack_hi = hi;
}

void cpu_get_thread_stack(gaddr_t *lo, gaddr_t *hi) {
    *lo = cur()->stack_lo;
    *hi = cur()->stack_hi;
}

void cpu_thread_attach(void) {
    struct dthread *t = cur();
    if (t->stack_hi) return;
    gaddr_t lo = gmem_map_anywhere(THREAD_GUEST_STACK, GPROT_RW, 0);
    if (!lo) fatal("dxcpu: no memory for a thread guest stack");
    t->stack_lo = lo;
    t->stack_hi = lo + THREAD_GUEST_STACK;
}

static _Noreturn void guest_fault(dxcpu_state *s, uint32_t why) {
    char where[256], dis[128];
    uint32_t eip = dxcpu_get_reg(s, DXR_FAULT_EIP);
    dxcpu_disasm(eip, dis, sizeof dis);
    fatal("dxcpu: %s at %s: %s (eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x)",
          why == DXCPU_UNIMPLEMENTED ? "unimplemented instruction" : why == DXCPU_HALT ? "hlt/int3/ud2" : "guest fault",
          macho_describe_addr(eip, where, sizeof where), dis, dxcpu_get_reg(s, 0), dxcpu_get_reg(s, 1),
          dxcpu_get_reg(s, 2), dxcpu_get_reg(s, 3), dxcpu_get_reg(s, 4), dxcpu_get_reg(s, 5), dxcpu_get_reg(s, 6),
          dxcpu_get_reg(s, 7));
}

// Run until `stop`, dispatching thunk calls on the way.
static void run_until(int depth, uint32_t stop) {
    struct dthread *t = cur();
    dxcpu_engine *e = t->eng[depth];
    dxcpu_state *s = t->st[depth];
    for (;;) {
        uint32_t why = dxcpu_run(e, s, stop);
        if (why == DXCPU_STOP) return;
        if (why != DXCPU_STUB) guest_fault(s, why);
        uint32_t eip = dxcpu_get_reg(s, DXR_EIP);
        if (eip >= RET_BASE) fatal("dxcpu: return marker %#x reached at the wrong depth", eip);
        struct guest_regs r;
        memset(&r, 0, sizeof r);
        r.thunk_id = (eip - STUB_BASE) / STUB_SIZE;
        r.eax = dxcpu_get_reg(s, DXR_EAX);
        r.ecx = dxcpu_get_reg(s, DXR_ECX);
        r.edx = dxcpu_get_reg(s, DXR_EDX);
        r.ebx = dxcpu_get_reg(s, DXR_EBX);
        r.esp = dxcpu_get_reg(s, DXR_ESP);
        r.ebp = dxcpu_get_reg(s, DXR_EBP);
        r.esi = dxcpu_get_reg(s, DXR_ESI);
        r.edi = dxcpu_get_reg(s, DXR_EDI);
        bool prev_in = t->in_thunk;
        uint32_t prev_hint = t->esp_hint;
        int prev_depth = t->depth;
        t->in_thunk = true;
        t->esp_hint = r.esp;
        t->depth = depth;
        struct guest_regs *prev_call = t->call;
        t->call = &r;
        host_dispatch(&r);
        t->call = prev_call;
        t->in_thunk = prev_in;
        t->esp_hint = prev_hint;
        t->depth = prev_depth;
        dxcpu_set_reg(s, DXR_EAX, (uint32_t)r.ret);
        dxcpu_set_reg(s, DXR_EDX, (uint32_t)(r.ret >> 32));
        dxcpu_set_reg(s, DXR_EBX, r.ebx);
        dxcpu_set_reg(s, DXR_EBP, r.ebp);
        dxcpu_set_reg(s, DXR_ESI, r.esi);
        dxcpu_set_reg(s, DXR_EDI, r.edi);
        if (r.fp_ret) dxcpu_x87_push(s, r.fret);
        dxcpu_set_reg(s, DXR_EIP, gread32(r.esp));
        dxcpu_set_reg(s, DXR_ESP, r.esp + 4 + r.pop);
    }
}

void cpu_call_ex(gaddr_t fn, int nargs, const uint32_t *args, bool fp_ret, struct cpu_call_result *out) {
    struct dthread *t = cur();
    cpu_thread_attach();
    uint32_t sp = t->in_thunk ? t->esp_hint - 512 : t->stack_hi - 64;
    sp -= 4 * (uint32_t)nargs;
    sp &= ~15u;
    for (int i = 0; i < nargs; i++) gwrite32(sp + 4 * i, args[i]);
    int depth = t->in_thunk ? t->depth + 1 : t->depth;
    gaddr_t marker = RET_BASE + (uint32_t)depth * STUB_SIZE;
    sp -= 4;
    gwrite32(sp, marker);
    ensure_depth(depth);
    dxcpu_state *s = t->st[depth];
    int prev_depth = t->depth;
    bool prev_in = t->in_thunk;
    t->depth = depth;
    t->in_thunk = false;
    dxcpu_set_reg(s, DXR_ESP, sp);
    dxcpu_set_reg(s, DXR_EIP, fn);
    run_until(depth, marker);
    t->depth = prev_depth;
    t->in_thunk = prev_in;
    if (out) {
        out->ret = dxcpu_get_reg(s, DXR_EAX) | ((uint64_t)dxcpu_get_reg(s, DXR_EDX) << 32);
        out->fret = fp_ret ? dxcpu_x87_pop(s) : 0;
    }
}

uint64_t cpu_call(gaddr_t fn, int nargs, const uint32_t *args) {
    struct cpu_call_result r;
    cpu_call_ex(fn, nargs, args, false, &r);
    return r.ret;
}

void cpu_start(gaddr_t eip, gaddr_t esp) {
    struct dthread *t = cur();
    ensure_depth(0);
    t->depth = 0;
    t->in_thunk = false;
    dxcpu_set_reg(t->st[0], DXR_ESP, esp);
    dxcpu_set_reg(t->st[0], DXR_EIP, eip);
    run_until(0, 0xFFFFFFFFu);
    fatal("dxcpu: program entry returned");
}

bool cpu_crash_regs(uint32_t *eip, uint32_t *esp, uint32_t *ebp, uint32_t regs[8]) {
    if (!dt || !dt->st[dt->depth]) return false;
    dxcpu_state *s = dt->st[dt->depth];
    for (int i = 0; i < 8; i++) regs[i] = dxcpu_get_reg(s, (uint32_t)i);
    *eip = dxcpu_get_reg(s, DXR_EIP);
    *esp = regs[4];
    *ebp = regs[5];
    return true;
}
