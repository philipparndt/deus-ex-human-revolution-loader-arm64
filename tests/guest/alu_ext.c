// adc/sbb, xadd/cmpxchg, shld/shrd, div/idiv, bt: results and flags (pushf and
// all setcc). Prints a hash to compare backends with real x86 (Rosetta build).
#include <stdint.h>
#include <stdio.h>

static uint32_t h = 2166136261u, hc[8];
static int cat;
static void mix(uint32_t v) { h = (h ^ v) * 16777619u; hc[cat] = (hc[cat] ^ v) * 16777619u; }
#define DEF 0x8d5u   /* OF SF ZF AF PF CF */
#define NOAF 0x8c5u
#define NO_OF 0x0c5u
#define CC_OF 0xc00fu
static const uint32_t vals[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678, 0xfffffffe, 0x80000001, 0x55555555};
#define NV (sizeof vals / sizeof vals[0])
static void t_adcl_add(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("addl %%edx, %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("addl %%edx, %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_sub(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("subl %%edx, %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("subl %%edx, %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_cmp(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("cmpl %%edx, %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("cmpl %%edx, %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_shr(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("testl %0, %0; shrl $1, %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("shrl $1, %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_inc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("incl %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("incl %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_test(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("testl %%edx, %%esi; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("testl %%edx, %%esi; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_stc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("stc; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("stc; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_clc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("clc; adcl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("clc; adcl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_adcl_imm(uint32_t a) {
    uint32_t r = a, fl;
    __asm__ volatile("stc; adcl $0x7fffffff, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & DEF);
}
static void t_adcl_mem(uint32_t a, uint32_t b) {
    volatile uint32_t m = a; uint32_t fl;
    __asm__ volatile("stc; adcl %2, %0; pushf; pop %1" : "+m"(m), "=r"(fl) : "r"(b) : "cc");
    mix(m); mix(fl & DEF);
}
static void t_sbbl_add(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("addl %%edx, %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("addl %%edx, %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_sub(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("subl %%edx, %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("subl %%edx, %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_cmp(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("cmpl %%edx, %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("cmpl %%edx, %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_shr(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("testl %0, %0; shrl $1, %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("shrl $1, %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_inc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("incl %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("incl %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_test(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("testl %%edx, %%esi; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("testl %%edx, %%esi; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_stc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("stc; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("stc; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_clc(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = a, fl, cc, s = c;
    __asm__ volatile("clc; sbbl %3, %0; pushf; pop %1" : "+r"(r), "=r"(fl), "+S"(s) : "r"(b), "d"(c) : "cc");
    mix(r); mix(fl & DEF);
    r = a; s = c;
    { uint8_t cb[16];
    __asm__ volatile("clc; sbbl %2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+S"(s) : "r"(b), "d"(c), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sbbl_imm(uint32_t a) {
    uint32_t r = a, fl;
    __asm__ volatile("stc; sbbl $0x7fffffff, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & DEF);
}
static void t_sbbl_mem(uint32_t a, uint32_t b) {
    volatile uint32_t m = a; uint32_t fl;
    __asm__ volatile("stc; sbbl %2, %0; pushf; pop %1" : "+m"(m), "=r"(fl) : "r"(b) : "cc");
    mix(m); mix(fl & DEF);
}
static void t_sbb_self(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__ volatile("cmpl %2, %1; sbbl %0, %0" : "=r"(r) : "r"(a), "r"(b) : "cc");
    mix(r);
}
static void t_xadd(uint32_t a, uint32_t b) {
    volatile uint32_t m = a; uint32_t r = b, fl, cc;
    __asm__ volatile("lock xaddl %0, %1; pushf; pop %2" : "+r"(r), "+m"(m), "=r"(fl) : : "cc");
    mix(r); mix(m); mix(fl & DEF);
    m = a; r = b;
    { uint8_t cb[16];
    __asm__ volatile("xaddl %0, %1; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r), "+m"(m) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc); mix(r); mix(m);
}
static void t_cmpxchg(uint32_t mem, uint32_t expect, uint32_t nv) {
    volatile uint32_t m = mem; uint32_t e = expect, fl;
    __asm__ volatile("lock cmpxchgl %3, %1; pushf; pop %2" : "+a"(e), "+m"(m), "=r"(fl) : "r"(nv) : "cc");
    /* Only ZF: Rosetta sets the other flags from dest - eax, real x86 (and
       QEMU, Bochs, this engine) from cmp eax, dest. */
    mix(e); mix(m); mix(fl & 0x40);
    m = mem; e = expect;
    __asm__ volatile("cmpxchgl %2, %1" : "+a"(e), "+m"(m) : "r"(nv) : "cc");
    mix(e); mix(m);
}
static void t_shldl_0(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $0, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $0, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_1(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $1, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NOAF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $1, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_shldl_2(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $2, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $2, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_3(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $3, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $3, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_4(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $4, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $4, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_5(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $5, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $5, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_6(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $6, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $6, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_7(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $7, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $7, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_8(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $8, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $8, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_9(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $9, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $9, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_10(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $10, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $10, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_11(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $11, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $11, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_12(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $12, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $12, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_13(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $13, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $13, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_14(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $14, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $14, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_15(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $15, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $15, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_16(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $16, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $16, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_17(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $17, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $17, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_18(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $18, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $18, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_19(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $19, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $19, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_20(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $20, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $20, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_21(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $21, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $21, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_22(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $22, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $22, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_23(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $23, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $23, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_24(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $24, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $24, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_25(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $25, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $25, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_26(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $26, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $26, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_27(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $27, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $27, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_28(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $28, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $28, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_29(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $29, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $29, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_30(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $30, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $30, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_31(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shldl $31, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shldl $31, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shldl_cl(uint32_t a, uint32_t b, uint32_t n) {
    uint32_t r = a, fl; volatile uint32_t m = a;
    __asm__ volatile("testl %0, %0; shldl %%cl, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b), "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? NOAF : NO_OF));
    __asm__ volatile("testl %%ecx, %%ecx; shldl %%cl, %1, %0" : "+m"(m) : "r"(b), "c"(n) : "cc");
    mix(m);
}
static void t_shrdl_0(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $0, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $0, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_1(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $1, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NOAF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $1, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_shrdl_2(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $2, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $2, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_3(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $3, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $3, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_4(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $4, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $4, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_5(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $5, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $5, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_6(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $6, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $6, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_7(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $7, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $7, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_8(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $8, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $8, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_9(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $9, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $9, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_10(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $10, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $10, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_11(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $11, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $11, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_12(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $12, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $12, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_13(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $13, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $13, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_14(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $14, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $14, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_15(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $15, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $15, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_16(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $16, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $16, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_17(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $17, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $17, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_18(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $18, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $18, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_19(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $19, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $19, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_20(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $20, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $20, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_21(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $21, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $21, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_22(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $22, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $22, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_23(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $23, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $23, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_24(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $24, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $24, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_25(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $25, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $25, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_26(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $26, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $26, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_27(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $27, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $27, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_28(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $28, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $28, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_29(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $29, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $29, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_30(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $30, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $30, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_31(uint32_t a, uint32_t b) {
    uint32_t r = a, fl, cc;
    __asm__ volatile("testl %0, %0; shrdl $31, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b) : "cc");
    mix(r); mix(fl & NO_OF);
    r = a;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrdl $31, %1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "r"(b), "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrdl_cl(uint32_t a, uint32_t b, uint32_t n) {
    uint32_t r = a, fl; volatile uint32_t m = a;
    __asm__ volatile("testl %0, %0; shrdl %%cl, %2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "r"(b), "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? NOAF : NO_OF));
    __asm__ volatile("testl %%ecx, %%ecx; shrdl %%cl, %1, %0" : "+m"(m) : "r"(b), "c"(n) : "cc");
    mix(m);
}
static void t_div(uint32_t hi, uint32_t lo, uint32_t d) {
    if (!d || hi >= d) return; /* #DE */
    uint32_t q = lo, r = hi;
    __asm__ volatile("divl %2" : "+a"(q), "+d"(r) : "r"(d) : "cc");
    mix(q); mix(r);
    volatile uint32_t m = d; q = lo; r = hi;
    __asm__ volatile("divl %2" : "+a"(q), "+d"(r) : "m"(m) : "cc");
    mix(q); mix(r);
}
static void t_idiv(int32_t lo, int32_t d) {
    if (!d || (lo == (int32_t)0x80000000 && d == -1)) return; /* #DE */
    int32_t q = lo, r = lo < 0 ? -1 : 0;
    __asm__ volatile("idivl %2" : "+a"(q), "+d"(r) : "r"(d) : "cc");
    mix((uint32_t)q); mix((uint32_t)r);
}
static void t_bt(uint32_t v, uint32_t n) {
    uint32_t cc, z1, z2; volatile uint32_t m = v;
    /* ZF from the cmp must survive bt */
    __asm__ volatile("cmpl %2, %2; btl %3, %4; setc %b0; setz %b1" : "=q"(cc), "=q"(z1) : "r"(v), "r"(n), "r"(v) : "cc");
    mix(cc & 1); mix(z1 & 1);
    __asm__ volatile("cmpl $1, %2; btl $13, %3; setc %b0; setz %b1" : "=q"(cc), "=q"(z2) : "r"(n), "m"(m) : "cc");
    mix(cc & 1); mix(z2 & 1);
    __asm__ volatile("addl $0, %2; btl $31, %2; setc %b0; setz %b1" : "=q"(cc), "=q"(z2) : "r"(v) : "cc");
    mix(cc & 1); mix(z2 & 1);
}
/* x86 allows locked operations on unaligned addresses (ARM LSE atomics do not). */
static void t_unaligned(uint32_t a, uint32_t b) {
    static uint8_t buf[64] __attribute__((aligned(16)));
    for (int off = 11; off < 18; off++) { /* 13..15 cross a 16-byte boundary */
        volatile uint32_t *p = (volatile uint32_t *)(buf + off);
        *p = a;
        uint32_t r = b, e = a, fl;
        __asm__ volatile("lock xaddl %0, %1; pushf; pop %2" : "+r"(r), "+m"(*p), "=r"(fl) : : "cc");
        mix(r); mix(*p); mix(fl & DEF);
        __asm__ volatile("lock cmpxchgl %3, %1; pushf; pop %2" : "+a"(e), "+m"(*p), "=r"(fl) : "r"(b) : "cc");
        mix(e); mix(*p); mix(fl & 0x40);
    }
}

int main(void) {
    for (unsigned i = 0; i < NV; i++)
        for (unsigned j = 0; j < NV; j++) {
            uint32_t a = vals[i], b = vals[j], c = vals[(i + 3 * j) % NV];
            cat = 0;
            t_adcl_add(a, b, c);
            t_adcl_sub(a, b, c);
            t_adcl_cmp(a, b, c);
            t_adcl_shr(a, b, c);
            t_adcl_inc(a, b, c);
            t_adcl_test(a, b, c);
            t_adcl_stc(a, b, c);
            t_adcl_clc(a, b, c);
            t_adcl_mem(a, b);
            cat = 1;
            t_sbbl_add(a, b, c);
            t_sbbl_sub(a, b, c);
            t_sbbl_cmp(a, b, c);
            t_sbbl_shr(a, b, c);
            t_sbbl_inc(a, b, c);
            t_sbbl_test(a, b, c);
            t_sbbl_stc(a, b, c);
            t_sbbl_clc(a, b, c);
            t_sbbl_mem(a, b);
            cat = 2;
            t_sbb_self(a, b);
            cat = 3;
            t_xadd(a, b);
            cat = 4;
            t_cmpxchg(a, b, c); t_cmpxchg(a, a, c);
            t_unaligned(a, b);
            cat = 5;
            t_shldl_0(a, b);
            t_shldl_1(a, b);
            t_shldl_2(a, b);
            t_shldl_3(a, b);
            t_shldl_4(a, b);
            t_shldl_5(a, b);
            t_shldl_6(a, b);
            t_shldl_7(a, b);
            t_shldl_8(a, b);
            t_shldl_9(a, b);
            t_shldl_10(a, b);
            t_shldl_11(a, b);
            t_shldl_12(a, b);
            t_shldl_13(a, b);
            t_shldl_14(a, b);
            t_shldl_15(a, b);
            t_shldl_16(a, b);
            t_shldl_17(a, b);
            t_shldl_18(a, b);
            t_shldl_19(a, b);
            t_shldl_20(a, b);
            t_shldl_21(a, b);
            t_shldl_22(a, b);
            t_shldl_23(a, b);
            t_shldl_24(a, b);
            t_shldl_25(a, b);
            t_shldl_26(a, b);
            t_shldl_27(a, b);
            t_shldl_28(a, b);
            t_shldl_29(a, b);
            t_shldl_30(a, b);
            t_shldl_31(a, b);
            t_shrdl_0(a, b);
            t_shrdl_1(a, b);
            t_shrdl_2(a, b);
            t_shrdl_3(a, b);
            t_shrdl_4(a, b);
            t_shrdl_5(a, b);
            t_shrdl_6(a, b);
            t_shrdl_7(a, b);
            t_shrdl_8(a, b);
            t_shrdl_9(a, b);
            t_shrdl_10(a, b);
            t_shrdl_11(a, b);
            t_shrdl_12(a, b);
            t_shrdl_13(a, b);
            t_shrdl_14(a, b);
            t_shrdl_15(a, b);
            t_shrdl_16(a, b);
            t_shrdl_17(a, b);
            t_shrdl_18(a, b);
            t_shrdl_19(a, b);
            t_shrdl_20(a, b);
            t_shrdl_21(a, b);
            t_shrdl_22(a, b);
            t_shrdl_23(a, b);
            t_shrdl_24(a, b);
            t_shrdl_25(a, b);
            t_shrdl_26(a, b);
            t_shrdl_27(a, b);
            t_shrdl_28(a, b);
            t_shrdl_29(a, b);
            t_shrdl_30(a, b);
            t_shrdl_31(a, b);
            cat = 6;
            for (uint32_t n = 0; n < 34; n++) { t_shldl_cl(a, b, n); t_shrdl_cl(a, b, n); t_bt(a, n); }
            cat = 7;
            t_div(a, b, c); t_div(b % (c | 1), a, c | 1); t_idiv((int32_t)a, (int32_t)b); t_idiv((int32_t)b, (int32_t)(c | 1));
        }
    for (unsigned i = 0; i < NV; i++) { t_adcl_imm(vals[i]); t_sbbl_imm(vals[i]); }
    for (int k = 0; k < 8; k++) printf("cat %d %08x\n", k, hc[k]);
    printf("alu_ext hash %08x\n", h);
    return 0;
}
