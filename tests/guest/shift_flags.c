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
    __asm__ volatile("shll $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $0, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $1, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc);
}
static void t_shll_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $2, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $3, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $4, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $5, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $6, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $7, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $8, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $9, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $10, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $11, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $12, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $13, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $14, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $15, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $16, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $17, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $18, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $19, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $20, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $21, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $22, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $23, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $24, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $25, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $26, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $27, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $28, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $29, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $30, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shll $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shll $31, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shll_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("shll %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? FLAGMASK : NO_OF));
}
static void t_shrl_0(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $0, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $1, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc);
}
static void t_shrl_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $2, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $3, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $4, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $5, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $6, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $7, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $8, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $9, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $10, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $11, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $12, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $13, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $14, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $15, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $16, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $17, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $18, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $19, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $20, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $21, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $22, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $23, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $24, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $25, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $26, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $27, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $28, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $29, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $30, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("shrl $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; shrl $31, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_shrl_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("shrl %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
    mix(r); mix(fl & ((n & 31) == 1 ? FLAGMASK : NO_OF));
}
static void t_sarl_0(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $0, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $0, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_1(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $1, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & FLAGMASK);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $1, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc);
}
static void t_sarl_2(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $2, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $2, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_3(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $3, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $3, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_4(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $4, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $4, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_5(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $5, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $5, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_6(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $6, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $6, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_7(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $7, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $7, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_8(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $8, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $8, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_9(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $9, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $9, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_10(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $10, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $10, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_11(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $11, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $11, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_12(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $12, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $12, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_13(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $13, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $13, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_14(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $14, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $14, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_15(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $15, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $15, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_16(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $16, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $16, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_17(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $17, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $17, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_18(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $18, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $18, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_19(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $19, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $19, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_20(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $20, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $20, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_21(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $21, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $21, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_22(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $22, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $22, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_23(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $23, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $23, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_24(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $24, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $24, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_25(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $25, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $25, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_26(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $26, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $26, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_27(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $27, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $27, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_28(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $28, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $28, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_29(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $29, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $29, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_30(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $30, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $30, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_31(uint32_t v) {
    uint32_t r = v, fl, cc = 0;
    __asm__ volatile("sarl $31, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : : "cc");
    mix(r); mix(fl & NO_OF);
    r = v;
    __asm__ volatile("xor %%ecx, %%ecx; cmp $1, %%ecx; sarl $31, %1; mov $0, %%eax; " "seto %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setno %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setb %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setae %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sete %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setne %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setbe %%al; shl $1, %%ecx; or %%eax, %%ecx; " "seta %%al; shl $1, %%ecx; or %%eax, %%ecx; " "sets %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setns %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setnp %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setl %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setge %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setle %%al; shl $1, %%ecx; or %%eax, %%ecx; " "setg %%al; shl $1, %%ecx; or %%eax, %%ecx; "  : "=c"(cc), "+r"(r) : : "eax", "cc");
    mix(cc & ~CC_OF);
}
static void t_sarl_cl(uint32_t v, uint32_t n) {
    uint32_t r = v, fl;
    __asm__ volatile("sarl %%cl, %0; pushf; pop %1" : "+r"(r), "=r"(fl) : "c"(n) : "cc");
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
