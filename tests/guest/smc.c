// Runtime-generated code: rewriting code through mprotect and re-mapping an
// address with new code must not run stale translations.
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "libc_check.h"

typedef int (*fn_t)(void);

static void emit(uint8_t *p, int v) { // mov eax, v; ret
    p[0] = 0xb8;
    memcpy(p + 1, &v, 4);
    p[5] = 0xc3;
}

int main(void) {
    uint8_t *code = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(code != MAP_FAILED, "mmap");
    emit(code, 1);
    mprotect(code, 4096, PROT_READ | PROT_EXEC);
    int a = 0;
    for (int i = 0; i < 3; i++) a = ((fn_t)code)(); // translated and cached
    CHECK(a == 1, "first code returns %d", a);
    mprotect(code, 4096, PROT_READ | PROT_WRITE);
    emit(code, 2);
    mprotect(code, 4096, PROT_READ | PROT_EXEC);
    int b = ((fn_t)code)();
    CHECK(b == 2, "rewritten code returns %d (stale translation?)", b);

    munmap(code, 4096);
    uint8_t *again = mmap(code, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    CHECK(again == code, "MAP_FIXED at the old address");
    emit(again, 3);
    int c = ((fn_t)again)();
    CHECK(c == 3, "code at a reused address returns %d", c);
    DONE();
}
