// CPU backend: native arm64 host, guest i386 code executed by Unicorn (QEMU's
// TCG JIT). First native backend; a dedicated translator may replace it.
//
// Guest memory is the 4 GB window at guest_base, mapped into every Unicorn
// engine with uc_mem_map_ptr, so guest and host share the same bytes.
// Thunk stubs live in a reserved guest range that is never executed: a code
// hook on that range dispatches to the host and emulates the `ret`.
// Unicorn is not reentrant, so nested host->guest calls (callbacks issued
// while a thunk runs) use another engine per nesting depth.
#include "cpu.h"
#include "log.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#define STUB_BASE 0xFFC00000u
#define MAX_STUBS 65536u
#define STUB_SIZE 16u
#define RET_BASE (STUB_BASE + MAX_STUBS * STUB_SIZE) // return markers, one per depth
#define MAX_DEPTH 32
#define THREAD_GUEST_STACK (1u << 20)

struct uthread {
    uc_engine *uc[MAX_DEPTH];
    int depth;          // current nesting depth (index of the running engine)
    uint32_t esp_hint;  // guest esp of the innermost thunk call
    bool in_thunk;
    gaddr_t stack_lo, stack_hi;
};

static _Thread_local struct uthread *ut;

static void fatal_uc(const char *what, uc_err e) { fatal("unicorn: %s: %s", what, uc_strerror(e)); }

static struct uthread *cur(void) {
    if (!ut) ut = calloc(1, sizeof *ut);
    return ut;
}

// --- x87 helpers -----------------------------------------------------------------------

struct f80 {
    uint64_t mant;
    uint16_t se;
} __attribute__((packed));

static double f80_to_double(const struct f80 *v) {
    int sign = v->se >> 15, exp = v->se & 0x7fff;
    if (exp == 0 && v->mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (v->mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double d = ldexp((double)v->mant, exp - 16383 - 63);
    return sign ? -d : d;
}

static struct f80 double_to_f80(double d) {
    struct f80 v = {0, signbit(d) ? 0x8000 : 0};
    if (isnan(d)) {
        v.se |= 0x7fff;
        v.mant = 0xC000000000000000ULL;
    } else if (isinf(d)) {
        v.se |= 0x7fff;
        v.mant = 0x8000000000000000ULL;
    } else if (d != 0) {
        int e;
        double m = frexp(fabs(d), &e);
        v.mant = (uint64_t)ldexp(m, 64);
        v.se |= (uint16_t)(e - 1 + 16383);
    }
    return v;
}

static void x87_push(uc_engine *uc, double d) {
    uint64_t fpsw = 0, tag = 0;
    uc_reg_read(uc, UC_X86_REG_FPSW, &fpsw);
    unsigned top = ((fpsw >> 11) & 7);
    top = (top - 1) & 7;
    fpsw = (fpsw & ~(7u << 11)) | (top << 11);
    uc_reg_write(uc, UC_X86_REG_FPSW, &fpsw);
    uc_reg_read(uc, UC_X86_REG_FPTAG, &tag);
    tag &= ~(3ull << (2 * top)); // valid
    uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    uint8_t buf[16] = {0};
    struct f80 f = double_to_f80(d);
    memcpy(buf, &f, 10);
    uc_reg_write(uc, UC_X86_REG_ST0, buf);
}

static double x87_pop(uc_engine *uc) {
    uint8_t buf[16] = {0};
    uc_reg_read(uc, UC_X86_REG_ST0, buf);
    struct f80 f;
    memcpy(&f, buf, 10);
    uint64_t fpsw = 0, tag = 0;
    uc_reg_read(uc, UC_X86_REG_FPSW, &fpsw);
    unsigned top = (fpsw >> 11) & 7;
    uc_reg_read(uc, UC_X86_REG_FPTAG, &tag);
    tag |= 3ull << (2 * top); // empty
    uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    top = (top + 1) & 7;
    fpsw = (fpsw & ~(7u << 11)) | (top << 11);
    uc_reg_write(uc, UC_X86_REG_FPSW, &fpsw);
    return f80_to_double(&f);
}

// --- thunk dispatch hook ----------------------------------------------------------------

static uint32_t rd(uc_engine *uc, int reg) {
    uint32_t v = 0;
    uc_reg_read(uc, reg, &v);
    return v;
}
static void wr(uc_engine *uc, int reg, uint32_t v) { uc_reg_write(uc, reg, &v); }

static void stub_hook(uc_engine *uc, uint64_t address, uint32_t size, void *user) {
    (void)size;
    (void)user;
    if (address >= RET_BASE) { // return marker of a host->guest call
        uc_emu_stop(uc);
        return;
    }
    struct uthread *t = cur();
    struct guest_regs r;
    memset(&r, 0, sizeof r);
    r.thunk_id = (uint32_t)((address - STUB_BASE) / STUB_SIZE);
    r.eax = rd(uc, UC_X86_REG_EAX);
    r.ecx = rd(uc, UC_X86_REG_ECX);
    r.edx = rd(uc, UC_X86_REG_EDX);
    r.ebx = rd(uc, UC_X86_REG_EBX);
    r.esp = rd(uc, UC_X86_REG_ESP);
    r.ebp = rd(uc, UC_X86_REG_EBP);
    r.esi = rd(uc, UC_X86_REG_ESI);
    r.edi = rd(uc, UC_X86_REG_EDI);
    bool prev_in = t->in_thunk;
    uint32_t prev_hint = t->esp_hint;
    t->in_thunk = true;
    t->esp_hint = r.esp;
    host_dispatch(&r);
    t->in_thunk = prev_in;
    t->esp_hint = prev_hint;
    wr(uc, UC_X86_REG_EAX, (uint32_t)r.ret);
    wr(uc, UC_X86_REG_EDX, (uint32_t)(r.ret >> 32));
    wr(uc, UC_X86_REG_EBX, r.ebx);
    wr(uc, UC_X86_REG_EBP, r.ebp);
    wr(uc, UC_X86_REG_ESI, r.esi);
    wr(uc, UC_X86_REG_EDI, r.edi);
    if (r.fp_ret) x87_push(uc, r.fret);
    // Emulate `ret` (+ callee-popped bytes) relative to the possibly rewritten esp.
    uint32_t ret_addr = gread32(r.esp);
    wr(uc, UC_X86_REG_ESP, r.esp + 4 + r.pop);
    wr(uc, UC_X86_REG_EIP, ret_addr);
}

static uc_engine *new_engine(void) {
    uc_engine *uc;
    uc_err e = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (e) fatal_uc("uc_open", e);
    e = uc_mem_map_ptr(uc, 0, 0x100000000ULL, UC_PROT_ALL, guest_base);
    if (e) fatal_uc("uc_mem_map_ptr", e);
    uc_hook h;
    e = uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)stub_hook, NULL, STUB_BASE, RET_BASE + MAX_DEPTH * STUB_SIZE);
    if (e) fatal_uc("uc_hook_add", e);
    // SSE enabled, x87 default control word
    uint64_t cr0 = 0x33, cr4 = 0x600; // protected mode without paging; OSFXSR|OSXMMEXCPT for SSE
    uc_reg_write(uc, UC_X86_REG_CR0, &cr0);
    uc_reg_write(uc, UC_X86_REG_CR4, &cr4);
    uint32_t mxcsr = 0x1f80;
    uc_reg_write(uc, UC_X86_REG_MXCSR, &mxcsr);
    return uc;
}

static uc_engine *engine(int depth) {
    struct uthread *t = cur();
    if (depth >= MAX_DEPTH) fatal("unicorn: host/guest call nesting too deep");
    if (!t->uc[depth]) t->uc[depth] = new_engine();
    return t->uc[depth];
}

// --- interface -------------------------------------------------------------------------------

void cpu_init(void) {
    if (!guest_base) fatal("unicorn backend requires a guest base");
    // Stub and return-marker range: mapped, filled with hlt (never really executed).
    if (!gmem_map_fixed(STUB_BASE, MAX_STUBS * STUB_SIZE + MAX_DEPTH * STUB_SIZE, GPROT_RW))
        fatal("unicorn: cannot reserve stub range");
    memset(g2h(STUB_BASE), 0xF4, MAX_STUBS * STUB_SIZE + MAX_DEPTH * STUB_SIZE);
    unsigned maj, min;
    uc_version(&maj, &min);
    LOGI("unicorn: backend ready (Unicorn %u.%u), stubs at %#x", maj, min, STUB_BASE);
}

gaddr_t cpu_make_stub(uint32_t id) {
    if (id >= MAX_STUBS) fatal("unicorn: too many thunk stubs");
    return STUB_BASE + id * STUB_SIZE;
}

void cpu_set_thread_stack(gaddr_t lo, gaddr_t hi) {
    struct uthread *t = cur();
    t->stack_lo = lo;
    t->stack_hi = hi;
}

void cpu_get_thread_stack(gaddr_t *lo, gaddr_t *hi) {
    struct uthread *t = cur();
    *lo = t->stack_lo;
    *hi = t->stack_hi;
}

void cpu_thread_attach(void) {
    struct uthread *t = cur();
    if (t->stack_hi) return;
    gaddr_t lo = gmem_map_anywhere(THREAD_GUEST_STACK, GPROT_RW, 0);
    if (!lo) fatal("unicorn: no memory for a thread guest stack");
    t->stack_lo = lo;
    t->stack_hi = lo + THREAD_GUEST_STACK;
}

void cpu_call_ex(gaddr_t fn, int nargs, const uint32_t *args, bool fp_ret, struct cpu_call_result *out) {
    struct uthread *t = cur();
    cpu_thread_attach();
    // Nested inside a thunk: continue below the caller's guest stack.
    uint32_t sp = t->in_thunk ? t->esp_hint - 512 : t->stack_hi - 64;
    sp -= 4 * (uint32_t)nargs;
    sp &= ~15u;
    for (int i = 0; i < nargs; i++) gwrite32(sp + 4 * i, args[i]);
    int depth = t->in_thunk ? t->depth + 1 : t->depth;
    gaddr_t marker = RET_BASE + (uint32_t)depth * STUB_SIZE;
    sp -= 4;
    gwrite32(sp, marker);

    uc_engine *uc = engine(depth);
    int prev_depth = t->depth;
    bool prev_in = t->in_thunk;
    t->depth = depth;
    t->in_thunk = false;
    wr(uc, UC_X86_REG_ESP, sp);
    uc_err e = uc_emu_start(uc, fn, marker, 0, 0);
    if (e) {
        uint32_t eip = rd(uc, UC_X86_REG_EIP);
        fatal("unicorn: guest fault at eip=%#x esp=%#x: %s", eip, rd(uc, UC_X86_REG_ESP), uc_strerror(e));
    }
    t->depth = prev_depth;
    t->in_thunk = prev_in;
    if (out) {
        out->ret = rd(uc, UC_X86_REG_EAX) | ((uint64_t)rd(uc, UC_X86_REG_EDX) << 32);
        out->fret = fp_ret ? x87_pop(uc) : 0;
    }
}

uint64_t cpu_call(gaddr_t fn, int nargs, const uint32_t *args) {
    struct cpu_call_result r;
    cpu_call_ex(fn, nargs, args, false, &r);
    return r.ret;
}

void cpu_start(gaddr_t eip, gaddr_t esp) {
    struct uthread *t = cur();
    uc_engine *uc = engine(0);
    t->depth = 0;
    t->in_thunk = false;
    wr(uc, UC_X86_REG_ESP, esp);
    uc_err e = uc_emu_start(uc, eip, 0, 0, 0);
    fatal("unicorn: program entry returned (eip=%#x): %s", rd(uc, UC_X86_REG_EIP), uc_strerror(e));
}

bool cpu_crash_regs(uint32_t *eip, uint32_t *esp, uint32_t *ebp, uint32_t regs[8]) {
    if (!ut || !ut->uc[ut->depth]) return false;
    uc_engine *uc = ut->uc[ut->depth];
    static const int r[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
    for (int i = 0; i < 8; i++) regs[i] = rd(uc, r[i]);
    *eip = rd(uc, UC_X86_REG_EIP);
    *esp = regs[4];
    *ebp = regs[5];
    return true;
}

void cpu_main_heartbeat(void) {} // watchdog: native backend only

void cpu_code_changed(gaddr_t addr, uint32_t len) { (void)addr; (void)len; }
