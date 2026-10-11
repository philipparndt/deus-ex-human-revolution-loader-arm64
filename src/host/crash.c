// Crash reporting: on SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP print the faulting
// guest location (image+offset), registers and a heuristic guest backtrace.
#include "crash.h"

#include "gmem.h"
#include "log.h"
#include "macho.h"
#include "cpu.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void describe(const char *what, uint64_t a) {
    char buf[256];
    Dl_info di;
    if (a < 0x100000000ULL && host_ptr_in_guest(guest_base + a))
        LOGE("  %-6s %#010llx  %s", what, a, macho_describe_addr((gaddr_t)a, buf, sizeof buf));
    else if (dladdr((void *)a, &di) && di.dli_fname)
        LOGE("  %-6s %#llx  %s`%s+%#llx", what, a, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
             di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? a - (uint64_t)di.dli_saddr : 0);
    else
        LOGE("  %-6s %#llx", what, a);
}

static void host_backtrace(void) {
    void *frames[32];
    int n = backtrace(frames, 32);
    LOGE("host backtrace:");
    for (int i = 2; i < n; i++) describe("  ", (uint64_t)frames[i]);
}

static bool readable(gaddr_t a) { return gmem_is_mapped(a & ~3u, 4); }

static void dump_code(gaddr_t eip) {
    if (!gmem_is_mapped(eip, 16)) return;
    char hex[64] = {0};
    for (int i = 0; i < 16; i++) snprintf(hex + 3 * i, 4, "%02x ", gread8(eip + (uint32_t)i));
    LOGE("  code at %#x: %s", eip, hex);
}

static void guest_backtrace(uint32_t esp, uint32_t ebp) {
    char buf[256];
    if (readable(esp) && readable(esp + 60)) {
        LOGE("stack at esp:");
        for (int i = 0; i < 16; i += 4)
            LOGE("  %08x: %08x %08x %08x %08x", esp + 4 * i, gread32(esp + 4 * i), gread32(esp + 4 * i + 4),
                 gread32(esp + 4 * i + 8), gread32(esp + 4 * i + 12));
    }
    LOGE("guest frames (ebp chain):");
    for (int i = 0; i < 32 && ebp && readable(ebp) && readable(ebp + 4); i++) {
        uint32_t ret = gread32(ebp + 4);
        LOGE("  #%-2d %#010x  %s", i, ret, macho_describe_addr(ret, buf, sizeof buf));
        uint32_t next = gread32(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
    LOGE("guest stack scan (values that point into code):");
    int shown = 0;
    for (uint32_t p = esp; p < esp + 4096 && shown < 24 && readable(p); p += 4) {
        uint32_t v = gread32(p);
        macho_describe_addr(v, buf, sizeof buf);
        if (strchr(buf, '+')) {
            LOGE("  [esp+%#05x] %#010x  %s", p - esp, v, buf);
            shown++;
        }
    }
}

static void handler(int sig, siginfo_t *si, void *uctx) {
    LOGE("=== fatal signal %d (%s) at address %p ===", sig, strsignal(sig), si->si_addr);
#if defined(__x86_64__)
    ucontext_t *uc = uctx;
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    describe("rip", ss->__rip);
    if (ss->__rip < 0x100000000ULL) dump_code((gaddr_t)ss->__rip);
    describe("fault", (uint64_t)si->si_addr);
    LOGE("  eax=%08llx ebx=%08llx ecx=%08llx edx=%08llx", ss->__rax & 0xffffffff, ss->__rbx & 0xffffffff,
         ss->__rcx & 0xffffffff, ss->__rdx & 0xffffffff);
    LOGE("  esi=%08llx edi=%08llx ebp=%08llx esp=%08llx cs=%llx", ss->__rsi & 0xffffffff, ss->__rdi & 0xffffffff,
         ss->__rbp & 0xffffffff, ss->__rsp & 0xffffffff, ss->__cs);
    if (ss->__rsp < 0x100000000ULL) guest_backtrace((uint32_t)ss->__rsp, (uint32_t)ss->__rbp);
#else
    (void)uctx;
    uint64_t fa = (uint64_t)si->si_addr;
    if (host_ptr_in_guest(si->si_addr)) LOGE("  fault in guest memory at guest address %#llx", fa - (uint64_t)guest_base);
    else describe("fault", fa);
    uint32_t eip, esp, ebp, gr[8];
    if (cpu_crash_regs(&eip, &esp, &ebp, gr)) {
        describe("eip*", eip);
        dump_code(eip);
        LOGE("  (eip* = start of the current translated block; registers may lag)");
        LOGE("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x", gr[0], gr[1], gr[2], gr[3],
             gr[4], gr[5], gr[6], gr[7]);
        guest_backtrace(esp, ebp);
    }
#endif
    host_backtrace();
    signal(sig, SIG_DFL);
    _exit(128 + sig);
}

void crash_install(void) {
    static char altstack[256 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

// Each thread needs its own alternate signal stack.
void crash_thread_init(void) {
    stack_t ss = {.ss_sp = malloc(128 * 1024), .ss_size = 128 * 1024, .ss_flags = 0};
    sigaltstack(&ss, NULL);
}
