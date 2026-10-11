// Proof of concept: execute 32-bit x86 code from a 64-bit x86_64 process.
// Under Rosetta 2 this tells us whether compatibility-mode code segments
// (installed via i386_set_ldt, as Wine/CrossOver do) are supported.
#include <architecture/i386/desc.h>
#include <i386/user_ldt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>

union ldt_entry {
    code_desc_t code;
    data_desc_t data;
};

extern uint32_t call32(uint32_t fn, uint32_t stack_top, uint32_t cs32);
uint64_t saved_rsp64;
extern void back64(void);

int main(void) {
    uint8_t *low = mmap((void *)0x10000000, 0x20000, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (low == MAP_FAILED) { perror("mmap"); return 1; }

    uint16_t cs64;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs64));
    uint16_t ss, ds;
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    printf("cs64=0x%x ss=0x%x ds=0x%x\n", cs64, ss, ds);

    union ldt_entry e;
    memset(&e, 0, sizeof e);
    e.code.limit00 = 0xffff;
    e.code.limit16 = 0xf;
    e.code.type = DESC_CODE_READ;
    e.code.dpl = 3;
    e.code.present = 1;
    e.code.opsz = DESC_CODE_32B;
    e.code.granular = DESC_GRAN_PAGE;
    int idx = -1;
    for (int i = 1; i < 32 && idx < 0; i++) idx = i386_set_ldt(i, &e, 1);
    if (idx < 0) { perror("i386_set_ldt"); return 1; }
    uint16_t cs32 = (uint16_t)((idx << 3) | 7);
    printf("ldt idx=%d cs32=0x%x\n", idx, cs32);

    // Layout in low memory:
    //   0x10000000  32-bit test function
    //   0x10000100  32-bit return gate: ljmp *[farptr]
    //   0x10000200  far pointer {offset32, cs64} -> 64-bit reentry stub
    //   0x10000300  64-bit reentry stub: movabs r11, back64; jmp r11
    //   0x10010000+ 32-bit stack
    uint8_t *fn = low;
    const uint8_t fnbytes[] = {
        0x8B, 0x44, 0x24, 0x04, // mov eax, [esp+4]
        0x48,                   // dec eax (in 64-bit mode this would be a REX.W prefix)
        0xC3,                   // ret
    };
    memcpy(fn, fnbytes, sizeof fnbytes);

    uint8_t *gate = low + 0x100;
    gate[0] = 0xFF; gate[1] = 0x2D; // jmp far [disp32]
    uint32_t farptr_addr = (uint32_t)(uintptr_t)(low + 0x200);
    memcpy(gate + 2, &farptr_addr, 4);

    uint8_t *farptr = low + 0x200;
    uint32_t reentry = (uint32_t)(uintptr_t)(low + 0x300);
    memcpy(farptr, &reentry, 4);
    memcpy(farptr + 4, &cs64, 2);

    uint8_t *re = low + 0x300;
    re[0] = 0x49; re[1] = 0xBB; // movabs r11, imm64
    uint64_t b = (uint64_t)(uintptr_t)&back64;
    memcpy(re + 2, &b, 8);
    re[10] = 0x41; re[11] = 0xFF; re[12] = 0xE3; // jmp r11

    uint8_t *gate2 = low + (getenv("SEPARATE") ? 0x8110 : 0x110);
    uint8_t *farptr2 = low + (getenv("SEPARATE") ? 0x8208 : 0x208);
    uint8_t *re2 = low + (getenv("SEPARATE") ? 0x8320 : 0x320);
    gate2[0] = 0xFF; gate2[1] = 0x2D;
    uint32_t fp2 = (uint32_t)(uintptr_t)farptr2;
    memcpy(gate2 + 2, &fp2, 4);
    uint32_t reentry2 = (uint32_t)(uintptr_t)re2;
    memcpy(farptr2, &reentry2, 4);
    memcpy(farptr2 + 4, &cs64, 2);
    memcpy(re2, re, 13);

    // 32-bit stack: [ret -> gate][arg]
    uint32_t *sp = (uint32_t *)(low + 0x1F000);
    sp[0] = (uint32_t)(uintptr_t)gate;
    sp[1] = 1000;

    printf("entering 32-bit code repeatedly...\n");
    fflush(stdout);
    uint32_t r = 0;
    for (int i = 0; i < 5; i++) {
        sp[0] = (uint32_t)(uintptr_t)((i & 1) ? gate2 : gate);
        sp[1] = 1000 + i;
        r = call32((uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)sp, cs32);
        printf("call %d returned eax=%u\n", i, r);
        if (getenv("POKE")) low[0x800] = (uint8_t)i; // write into the page holding gates
        fflush(stdout);
    }
    return 0;
}
