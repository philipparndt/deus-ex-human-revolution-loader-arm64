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

static void make_all_stubs(void);

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


#define STUB_SIZE 16
#define THREAD_GUEST_STACK (1u << 20)

static _Thread_local gaddr_t thread_stack_lo, thread_stack_hi;

static void write_far_gate(gaddr_t gate32, gaddr_t farptr, gaddr_t tramp64, void (*target)(void), uint16_t cs64) {
    (void)farptr;
    // 32-bit: jmp far ptr16:32 (direct form; the indirect `jmp far [m16:32]`
    // is mistranslated by Rosetta when the same gate is taken twice in a row)
    gwrite8(gate32, 0xEA);
    gwrite32(gate32 + 1, tramp64);
    gwrite16(gate32 + 5, cs64);
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

    // 32-bit gates and 64-bit trampolines live on separate pages: Rosetta
    // mistranslates mode switches when both kinds of code share a page.
    gate_page = gmem_map_anywhere(3 * GPAGE_SIZE, GPROT_RWX, 0);
    if (!gate_page) fatal("rosetta: no memory for gates");
    gate_host32 = gate_page;
    gate_ret32 = gate_page + 0x10;
    write_far_gate(gate_host32, 0, gate_page + GPAGE_SIZE, rosetta_host_entry, cs64);
    write_far_gate(gate_ret32, 0, gate_page + 2 * GPAGE_SIZE, rosetta_return64, cs64);
    make_all_stubs();
    LOGI("rosetta: cs32=%#llx cs64=%#x gates at %#x", rosetta_cs32, cs64, gate_page);
}

// All stubs are written once at startup: writing to a page with translated
// guest code later on breaks Rosetta's mode switching (see macho_obj.c).
#define MAX_STUBS 65536
static gaddr_t stub_base;

static void make_all_stubs(void) {
    stub_base = gmem_map_anywhere(MAX_STUBS * STUB_SIZE, GPROT_RWX, 0);
    if (!stub_base) fatal("rosetta: no memory for stubs");
    for (uint32_t id = 0; id < MAX_STUBS; id++) {
        gaddr_t s = stub_base + id * STUB_SIZE;
        gwrite8(s, 0xB8); // mov eax, id
        gwrite32(s + 1, id);
        gwrite8(s + 5, 0xE9); // jmp rel32 gate_host32
        gwrite32(s + 6, gate_host32 - (s + 10));
        gwrite8(s + 10, 0xCC);
    }
}

gaddr_t cpu_make_stub(uint32_t id) {
    if (id >= MAX_STUBS) fatal("rosetta: too many thunk stubs");
    return stub_base + id * STUB_SIZE;
}

void cpu_set_thread_stack(gaddr_t lo, gaddr_t hi) {
    thread_stack_lo = lo;
    thread_stack_hi = hi;
}

void cpu_get_thread_stack(gaddr_t *lo, gaddr_t *hi) {
    *lo = thread_stack_lo;
    *hi = thread_stack_hi;
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

bool cpu_crash_regs(uint32_t *eip, uint32_t *esp, uint32_t *ebp, uint32_t regs[8]) {
    (void)eip, (void)esp, (void)ebp, (void)regs;
    return false; // the signal context holds the guest registers directly
}
