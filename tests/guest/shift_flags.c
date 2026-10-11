// Shifts/rotates: results and flags (pushf and all setcc) for every count,
// immediate and CL. Prints a hash to compare backends with real x86 (Rosetta).
#include <stdint.h>
#include <stdio.h>

static uint32_t h = 2166136261u;
static void mix(uint32_t v) { h = (h ^ v) * 16777619u; }
#define FLAGMASK 0x8c5u /* OF SF ZF PF CF (AF undefined) */
#define NO_OF 0x0c5u    /* OF is undefined for counts other than 1 */
#define CC_OF 0xc00fu   /* setcc bits that read OF: o no l ge le g */
static void t_shll_0(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $0, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_shll_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $3, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $4, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $5, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $6, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $7, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $8, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $9, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $10, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $11, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $12, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $13, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $14, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $15, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $16, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $17, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $18, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $19, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $20, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $21, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $22, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $23, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $24, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $25, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $26, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $27, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $28, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $29, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $30, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shll $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shll $31, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shll_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("testl %0, %0; shll %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? FLAGMASK : NO_OF));
}
static void t_shrl_0(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $0, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_shrl_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $3, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $4, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $5, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $6, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $7, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $8, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $9, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $10, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $11, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $12, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $13, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $14, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $15, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $16, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $17, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $18, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $19, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $20, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $21, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $22, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $23, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $24, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $25, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $26, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $27, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $28, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $29, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $30, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; shrl $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; shrl $31, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_shrl_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("testl %0, %0; shrl %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? FLAGMASK : NO_OF));
}
static void t_sarl_0(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $0, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $1, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc);
}
static void t_sarl_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $2, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $3, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $4, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $5, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $6, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $7, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $8, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $9, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $10, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $11, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $12, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $13, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $14, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $15, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $16, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $17, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $18, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $19, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $20, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $21, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $22, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $23, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $24, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $25, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $26, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $27, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $28, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $29, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $30, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("testl %0, %0; sarl $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    { uint8_t cb[16];
    __asm__ volatile("xorl %%eax, %%eax; cmpl $1, %%eax; sarl $31, %0; " "seto 0(%%edi); " "setno 1(%%edi); " "setb 2(%%edi); " "setae 3(%%edi); " "sete 4(%%edi); " "setne 5(%%edi); " "setbe 6(%%edi); " "seta 7(%%edi); " "sets 8(%%edi); " "setns 9(%%edi); " "setp 10(%%edi); " "setnp 11(%%edi); " "setl 12(%%edi); " "setge 13(%%edi); " "setle 14(%%edi); " "setg 15(%%edi); "  : "+r"(r) : "D"(cb) : "eax", "cc", "memory");
      cc = 0; for (int k = 0; k < 16; k++) cc = cc << 1 | cb[k]; }
    mix(cc & ~CC_OF);
}
static void t_sarl_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("testl %0, %0; sarl %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? FLAGMASK : NO_OF));
}
static void t_rot(uint32_t v, uint32_t n) {
    uint32_t a = v, b = v, c = v, d = v;
    __asm__ volatile("roll $7, %0; rorl $13, %1; roll %%cl, %2; rorl %%cl, %3" : "+r"(a), "+r"(b), "+r"(c), "+r"(d) : "c"(n) : "cc");
    mix(a); mix(b); mix(c); mix(d);
}
static const uint32_t vals[] = {0x00000000, 0x00000001, 0x80000000, 0x7fffffff, 0xffffffff, 0x12345678, 0xdeadbeef, 0x40000001};
int main(void) {
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        t_shll_0(vals[i]);
        t_shll_1(vals[i]);
        t_shll_2(vals[i]);
        t_shll_3(vals[i]);
        t_shll_4(vals[i]);
        t_shll_5(vals[i]);
        t_shll_6(vals[i]);
        t_shll_7(vals[i]);
        t_shll_8(vals[i]);
        t_shll_9(vals[i]);
        t_shll_10(vals[i]);
        t_shll_11(vals[i]);
        t_shll_12(vals[i]);
        t_shll_13(vals[i]);
        t_shll_14(vals[i]);
        t_shll_15(vals[i]);
        t_shll_16(vals[i]);
        t_shll_17(vals[i]);
        t_shll_18(vals[i]);
        t_shll_19(vals[i]);
        t_shll_20(vals[i]);
        t_shll_21(vals[i]);
        t_shll_22(vals[i]);
        t_shll_23(vals[i]);
        t_shll_24(vals[i]);
        t_shll_25(vals[i]);
        t_shll_26(vals[i]);
        t_shll_27(vals[i]);
        t_shll_28(vals[i]);
        t_shll_29(vals[i]);
        t_shll_30(vals[i]);
        t_shll_31(vals[i]);
        for (uint32_t n = 0; n < 40; n++) t_shll_cl(vals[i], n);
        t_shrl_0(vals[i]);
        t_shrl_1(vals[i]);
        t_shrl_2(vals[i]);
        t_shrl_3(vals[i]);
        t_shrl_4(vals[i]);
        t_shrl_5(vals[i]);
        t_shrl_6(vals[i]);
        t_shrl_7(vals[i]);
        t_shrl_8(vals[i]);
        t_shrl_9(vals[i]);
        t_shrl_10(vals[i]);
        t_shrl_11(vals[i]);
        t_shrl_12(vals[i]);
        t_shrl_13(vals[i]);
        t_shrl_14(vals[i]);
        t_shrl_15(vals[i]);
        t_shrl_16(vals[i]);
        t_shrl_17(vals[i]);
        t_shrl_18(vals[i]);
        t_shrl_19(vals[i]);
        t_shrl_20(vals[i]);
        t_shrl_21(vals[i]);
        t_shrl_22(vals[i]);
        t_shrl_23(vals[i]);
        t_shrl_24(vals[i]);
        t_shrl_25(vals[i]);
        t_shrl_26(vals[i]);
        t_shrl_27(vals[i]);
        t_shrl_28(vals[i]);
        t_shrl_29(vals[i]);
        t_shrl_30(vals[i]);
        t_shrl_31(vals[i]);
        for (uint32_t n = 0; n < 40; n++) t_shrl_cl(vals[i], n);
        t_sarl_0(vals[i]);
        t_sarl_1(vals[i]);
        t_sarl_2(vals[i]);
        t_sarl_3(vals[i]);
        t_sarl_4(vals[i]);
        t_sarl_5(vals[i]);
        t_sarl_6(vals[i]);
        t_sarl_7(vals[i]);
        t_sarl_8(vals[i]);
        t_sarl_9(vals[i]);
        t_sarl_10(vals[i]);
        t_sarl_11(vals[i]);
        t_sarl_12(vals[i]);
        t_sarl_13(vals[i]);
        t_sarl_14(vals[i]);
        t_sarl_15(vals[i]);
        t_sarl_16(vals[i]);
        t_sarl_17(vals[i]);
        t_sarl_18(vals[i]);
        t_sarl_19(vals[i]);
        t_sarl_20(vals[i]);
        t_sarl_21(vals[i]);
        t_sarl_22(vals[i]);
        t_sarl_23(vals[i]);
        t_sarl_24(vals[i]);
        t_sarl_25(vals[i]);
        t_sarl_26(vals[i]);
        t_sarl_27(vals[i]);
        t_sarl_28(vals[i]);
        t_sarl_29(vals[i]);
        t_sarl_30(vals[i]);
        t_sarl_31(vals[i]);
        for (uint32_t n = 0; n < 40; n++) t_sarl_cl(vals[i], n);
        for (uint32_t n = 0; n < 40; n++) t_rot(vals[i], n);
    }
    printf("shift_flags hash %08x\n", h);
    return 0;
}
