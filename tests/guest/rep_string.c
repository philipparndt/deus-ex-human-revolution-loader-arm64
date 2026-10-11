// rep movs/stos semantics (the engine copies/fills in bulk where that is exact).
#include <string.h>

#include "libc_check.h"

static void rep_movsb(void *d, const void *s, unsigned n) {
    __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void rep_movsd(void *d, const void *s, unsigned n) {
    __asm__ volatile("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void rep_stosw(void *d, unsigned short v, unsigned n) {
    __asm__ volatile("cld; rep stosw" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
static void rep_stosd(void *d, unsigned v, unsigned n) {
    __asm__ volatile("cld; rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

int main(void) {
    char b[32];
    strcpy(b, "ab..............");
    rep_movsb(b + 2, b, 10); // forward overlap replicates the pattern on x86
    CHECK(memcmp(b, "abababababab", 12) == 0, "movsb forward overlap: %.12s", b);
    strcpy(b, "0123456789abcdef");
    rep_movsb(b, b + 4, 8);
    CHECK(memcmp(b, "456789ab89abcdef", 16) == 0, "movsb backward overlap: %.16s", b);
    unsigned src[4] = {1, 2, 3, 4}, dst[4] = {0};
    rep_movsd(dst, src, 4);
    CHECK(dst[0] == 1 && dst[3] == 4, "movsd");
    unsigned short w[5] = {0};
    rep_stosw(w, 0xBEEF, 4);
    CHECK(w[0] == 0xBEEF && w[3] == 0xBEEF && w[4] == 0, "stosw");
    unsigned dd[5] = {0};
    rep_stosd(dd, 0x12345678, 4);
    CHECK(dd[0] == 0x12345678 && dd[3] == 0x12345678 && dd[4] == 0, "stosd");
    unsigned n = 0;
    rep_movsb(b, b + 1, n);
    CHECK(memcmp(b, "456789ab", 8) == 0, "zero count");
    DONE();
}
