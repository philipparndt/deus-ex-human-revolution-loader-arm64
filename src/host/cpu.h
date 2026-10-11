// CPU backend interface: executes guest i386 code.
//
// Backends: cpu_rosetta (x86_64 host, 32-bit code runs in compatibility mode
// under Rosetta 2; feasibility check only) and later a native arm64 translator.
// Everything outside the backend only uses this interface.
#pragma once
#include "gmem.h"

// Guest register state as seen by a host thunk. On entry esp points at the
// return address, so argument i is at esp + 4 + 4*i.
struct guest_regs {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t thunk_id;
    uint32_t pop;    // extra bytes the callee pops (e.g. 4 for struct-return pointer)
    uint32_t fp_ret; // nonzero: return fret in st(0)
    uint32_t pad_;
    uint64_t ret;    // returned in edx:eax
    double fret;
};

struct cpu_call_result {
    uint64_t ret; // edx:eax
    double fret;  // st(0), only meaningful if the callee returns a float
};

#if defined(CPU_BACKEND_ROSETTA)
#define CPU_DIRECT_LOW true           // guest addresses are host addresses
#define CPU_EXECUTES_GUEST_PAGES true // guest code runs directly from guest memory
#else
#define CPU_DIRECT_LOW false
#define CPU_EXECUTES_GUEST_PAGES false
#endif

void cpu_init(void);
// Per-thread setup for a host thread that will run guest code; allocates a
// guest stack if needed. Safe to call repeatedly.
void cpu_thread_attach(void);
// Use the given guest memory as the current thread's guest stack.
void cpu_set_thread_stack(gaddr_t lo, gaddr_t hi);
void cpu_get_thread_stack(gaddr_t *lo, gaddr_t *hi);

// Call a guest cdecl function with 32-bit stack arguments.
uint64_t cpu_call(gaddr_t fn, int nargs, const uint32_t *args);
void cpu_call_ex(gaddr_t fn, int nargs, const uint32_t *args, bool fp_ret, struct cpu_call_result *out);

// Jump to the program entry point with esp = initial stack. Does not return.
_Noreturn void cpu_start(gaddr_t eip, gaddr_t esp);

// For crash reports: fill guest registers of the current thread if the backend
// knows them (returns false otherwise).
bool cpu_crash_regs(uint32_t *eip, uint32_t *esp, uint32_t *ebp, uint32_t regs[8]);

// Create a guest code stub that, when called, dispatches to host_dispatch()
// with r->thunk_id = id.
gaddr_t cpu_make_stub(uint32_t id);

// Implemented by the thunk layer, invoked by the backend.
void host_dispatch(struct guest_regs *r);

// Convenience wrappers.
static inline uint32_t gcall0(gaddr_t fn) { return (uint32_t)cpu_call(fn, 0, NULL); }
static inline uint32_t gcall1(gaddr_t fn, uint32_t a) { return (uint32_t)cpu_call(fn, 1, &a); }
static inline uint32_t gcall2(gaddr_t fn, uint32_t a, uint32_t b) {
    uint32_t v[2] = {a, b};
    return (uint32_t)cpu_call(fn, 2, v);
}
static inline uint32_t gcall3(gaddr_t fn, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t v[3] = {a, b, c};
    return (uint32_t)cpu_call(fn, 3, v);
}

// Watchdog (DXH_WATCHDOG, native backend): the main thread is alive.
void cpu_main_heartbeat(void);
