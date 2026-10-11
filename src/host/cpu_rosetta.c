// CPU backend: run i386 code in x86 compatibility mode under Rosetta 2.
//
// Feasibility-check backend only (Rosetta is being phased out). Requires an
// x86_64 host binary linked with a small __PAGEZERO so the low 4 GB are free.
#include "cpu.h"
#include "log.h"

#include <architecture/i386/desc.h>
#include <i386/user_ldt.h>
#include <pthread.h>
#include <string.h>

union ldt_entry {
    code_desc_t code;
    data_desc_t data;
};

struct rosetta_frame {
    uint64_t saved_rsp;
    struct rosetta_frame *prev;
    uint64_t ret;
    double fret;
    uint32_t fp;
};

extern void rosetta_host_entry(void);
extern void rosetta_return64(void);
extern void rosetta_enter32(uint32_t eip, uint32_t esp, struct rosetta_frame *frame);
extern _Noreturn void rosetta_jump32(uint32_t eip, uint32_t esp);

uint64_t rosetta_cs32;
_Thread_local struct rosetta_frame *rosetta_cur_frame;

static gaddr_t gate_page;   // trampolines
static gaddr_t gate_host32; // 32-bit: far jump to host entry
static gaddr_t gate_ret32;  // 32-bit: far jump to return path

static gaddr_t stub_chunk;
static uint32_t stub_used, stub_cap;
static pthread_mutex_t stub_lock = PTHREAD_MUTEX_INITIALIZER;

#define STUB_SIZE 16
#define STUB_CHUNK 0x40000
#define THREAD_GUEST_STACK (1u << 20)

static _Thread_local gaddr_t thread_stack_lo, thread_stack_hi;

static void write_far_gate(gaddr_t gate32, gaddr_t farptr, gaddr_t tramp64, void (*target)(void), uint16_t cs64) {
    // 32-bit: jmp far [farptr]
    gwrite8(gate32, 0xFF);
    gwrite8(gate32 + 1, 0x2D);
    gwrite32(gate32 + 2, farptr);
    // m16:32 far pointer to the 64-bit trampoline
    gwrite32(farptr, tramp64);
    gwrite16(farptr + 4, cs64);
    // 64-bit: movabs r11, target; jmp r11
    gwrite8(tramp64, 0x49);
    gwrite8(tramp64 + 1, 0xBB);
    uint64_t t = (uint64_t)(uintptr_t)target;
    memcpy(g2h(tramp64 + 2), &t, 8);
    gwrite8(tramp64 + 10, 0x41);
    gwrite8(tramp64 + 11, 0xFF);
    gwrite8(tramp64 + 12, 0xE3);
}

void cpu_init(void) {
    if (guest_base != NULL) fatal("rosetta backend requires guest_base == 0");
    union ldt_entry e;
    memset(&e, 0, sizeof e);
    e.code.limit00 = 0xffff;
    e.code.limit16 = 0xf;
    e.code.type = DESC_CODE_READ;
    e.code.dpl = 3;
    e.code.present = 1;
    e.code.opsz = DESC_CODE_32B;
    e.code.granular = DESC_GRAN_PAGE;
    // Rosetta rejects LDT_AUTO_ALLOC but accepts explicit slots.
    int idx = -1;
    for (int i = 1; i < 64 && idx < 0; i++) idx = i386_set_ldt(i, &e, 1);
    if (idx < 0) fatal("rosetta: i386_set_ldt failed; 32-bit code segments unavailable");
    rosetta_cs32 = (uint64_t)((idx << 3) | 7);

    uint16_t cs64;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs64));

    gate_page = gmem_map_anywhere(GPAGE_SIZE, GPROT_RWX, 0);
    if (!gate_page) fatal("rosetta: no memory for gates");
    gate_host32 = gate_page;
    gate_ret32 = gate_page + 0x10;
    write_far_gate(gate_host32, gate_page + 0x40, gate_page + 0x80, rosetta_host_entry, cs64);
    write_far_gate(gate_ret32, gate_page + 0x48, gate_page + 0xA0, rosetta_return64, cs64);
    LOGI("rosetta: cs32=%#llx cs64=%#x gates at %#x", rosetta_cs32, cs64, gate_page);
}

gaddr_t cpu_make_stub(uint32_t id) {
    pthread_mutex_lock(&stub_lock);
    if (stub_used + STUB_SIZE > stub_cap) {
        stub_chunk = gmem_map_anywhere(STUB_CHUNK, GPROT_RWX, 0);
        if (!stub_chunk) fatal("rosetta: no memory for stubs");
        stub_used = 0;
        stub_cap = STUB_CHUNK;
    }
    gaddr_t s = stub_chunk + stub_used;
    stub_used += STUB_SIZE;
    pthread_mutex_unlock(&stub_lock);
    gwrite8(s, 0xB8); // mov eax, id
    gwrite32(s + 1, id);
    gwrite8(s + 5, 0xE9); // jmp rel32 gate_host32
    gwrite32(s + 6, gate_host32 - (s + 10));
    gwrite8(s + 10, 0xCC);
    return s;
}

void cpu_set_thread_stack(gaddr_t lo, gaddr_t hi) {
    thread_stack_lo = lo;
    thread_stack_hi = hi;
}

void cpu_thread_attach(void) {
    if (thread_stack_hi) return;
    gaddr_t lo = gmem_map_anywhere(THREAD_GUEST_STACK, GPROT_RW, 0);
    if (!lo) fatal("rosetta: no memory for a thread guest stack");
    thread_stack_lo = lo;
    thread_stack_hi = lo + THREAD_GUEST_STACK;
}

// Pick the guest stack for a host->guest call: if we are already running on a
// guest stack (nested inside a thunk), continue below the current position.
static uint32_t current_guest_sp(void) {
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (sp < 0x100000000ULL) return (uint32_t)(sp - 512);
    cpu_thread_attach();
    // Not nested: the thread stack is free from the top (nested host-thread calls
    // happen on the guest stack and take the branch above).
    return thread_stack_hi - 64;
}

void cpu_call_ex(gaddr_t fn, int nargs, const uint32_t *args, bool fp_ret, struct cpu_call_result *out) {
    uint32_t sp = current_guest_sp();
    sp -= 4 * (uint32_t)nargs;
    sp &= ~15u; // stack aligned at the call site
    for (int i = 0; i < nargs; i++) gwrite32(sp + 4 * i, args[i]);
    sp -= 4;
    gwrite32(sp, gate_ret32);

    struct rosetta_frame frame = {0};
    frame.prev = rosetta_cur_frame;
    frame.fp = fp_ret;
    rosetta_cur_frame = &frame;
    rosetta_enter32(fn, sp, &frame);
    rosetta_cur_frame = frame.prev;
    if (out) {
        out->ret = frame.ret;
        out->fret = frame.fret;
    }
}

uint64_t cpu_call(gaddr_t fn, int nargs, const uint32_t *args) {
    struct cpu_call_result r;
    cpu_call_ex(fn, nargs, args, false, &r);
    return r.ret;
}

void cpu_start(gaddr_t eip, gaddr_t esp) { rosetta_jump32(eip, esp); }
