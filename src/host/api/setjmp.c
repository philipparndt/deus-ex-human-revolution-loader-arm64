// setjmp/longjmp: the jmp_buf layout is private to libc, so we use our own
// (it fits the guest's 72-byte jmp_buf / 76-byte sigjmp_buf):
//   [0] ebx [1] esi [2] edi [3] ebp [4] esp after return [5] return eip [6] magic
#include "common.h"

#define JB_MAGIC 0x444A4D50u

static void do_setjmp(struct guest_regs *r) {
    gaddr_t jb = ARG(0);
    gwrite32(jb + 0, r->ebx);
    gwrite32(jb + 4, r->esi);
    gwrite32(jb + 8, r->edi);
    gwrite32(jb + 12, r->ebp);
    gwrite32(jb + 16, r->esp + 4);
    gwrite32(jb + 20, gread32(r->esp));
    gwrite32(jb + 24, JB_MAGIC);
    RET(0);
}

static void do_longjmp(struct guest_regs *r) {
    gaddr_t jb = ARG(0);
    uint32_t val = ARG(1);
    if (gread32(jb + 24) != JB_MAGIC) fatal("longjmp: invalid jmp_buf %#x", jb);
    r->ebx = gread32(jb + 0);
    r->esi = gread32(jb + 4);
    r->edi = gread32(jb + 8);
    r->ebp = gread32(jb + 12);
    // The backend returns to [esp] and sets esp += 4.
    r->esp = gread32(jb + 16) - 4;
    gwrite32(r->esp, gread32(jb + 20));
    r->pop = 0;
    RET(val ? val : 1);
}

THUNK(setjmp) { do_setjmp(r); }
THUNK(_setjmp) { do_setjmp(r); }
THUNK(sigsetjmp) { do_setjmp(r); }
THUNK(longjmp) { do_longjmp(r); }
THUNK(_longjmp) { do_longjmp(r); }
THUNK(siglongjmp) { do_longjmp(r); }
