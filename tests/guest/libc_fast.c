// Guest-side libc fast paths (src/guest/libfast): correctness and call cost.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "libc_check.h"

static volatile size_t vn; // defeat builtin inlining
static double now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec * 1e-6; }

static uint64_t ref_udiv(uint64_t n, uint64_t d) { // reference: long division
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1ull << i; } }
    return q;
}

int main(void) {
    char a[64], b[64];
    for (int i = 0; i < 64; i++) a[i] = b[i] = (char)(i * 7);
    vn = 64; CHECK(memcmp(a, b, vn) == 0, "%s", "memcmp(a, b, vn) == 0");
    b[37] = 1; vn = 64; CHECK(memcmp(a, b, vn) != 0, "%s", "memcmp(a, b, vn) != 0");
    vn = 37; CHECK(memcmp(a, b, vn) == 0, "%s", "memcmp(a, b, vn) == 0");
    unsigned char x[2] = {0x80, 0}, y[2] = {0x01, 0};
    vn = 1; CHECK(memcmp(x, y, vn) > 0, "%s", "memcmp(x, y, vn) > 0"); // unsigned compare
    char buf[32] = "0123456789abcdef";
    vn = 10; memmove(buf + 2, buf, vn); CHECK(memcmp(buf, "010123456789", 12) == 0, "%s", "memcmp(buf, \"010123456789\", 12) == 0"); // overlap forward
    strcpy(buf, "0123456789abcdef");
    vn = 10; memmove(buf, buf + 3, vn); CHECK(memcmp(buf, "3456789abc", 10) == 0, "%s", "memcmp(buf, \"3456789abc\", 10) == 0"); // overlap backward
    char c[33]; vn = 31; memset(c + 1, 'z', vn); c[32] = 0; c[0] = 'a';
    CHECK(c[1] == 'z' && c[31] == 'z' && strlen(c) == 32, "%s", "c[1] == 'z' && c[31] == 'z' && strlen(c) == 32");
    vn = 31; bzero(c + 1, vn); CHECK(c[0] == 'a' && c[1] == 0 && c[31] == 0, "%s", "c[0] == 'a' && c[1] == 0 && c[31] == 0");
    char d[16]; vn = 13; memcpy(d + 1, "hello, world", vn); CHECK(strcmp(d + 1, "hello, world") == 0, "%s", "strcmp(d + 1, \"hello, world\") == 0");
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("", "") == 0, "%s", "strcmp(\"abc\", \"abd\") < 0 && strcmp(\"b\", \"a\") > 0 && strcmp(\"\", \"\") == 0");
    CHECK(strcmp("\x80", "a") > 0, "%s", "strcmp(\"\\x80\", \"a\") > 0"); // unsigned
    CHECK(strncmp("abcx", "abcy", 3) == 0 && strncmp("ab", "abc", 5) < 0, "%s", "strncmp(\"abcx\", \"abcy\", 3) == 0 && strncmp(\"ab\", \"abc\", 5) < 0");
    volatile float fn = NAN, fi = INFINITY, f1 = 1.5f;
    volatile double dn = NAN, di = -INFINITY;
    CHECK(isnan(fn) && !isnan(f1) && isnan(dn), "%s", "isnan(fn) && !isnan(f1) && isnan(dn)");
    CHECK(isinf(fi) && isinf(di) && !isinf(f1) && !isfinite(fi) && isfinite(f1), "%s", "isinf(fi) && isinf(di) && !isinf(f1) && !isfinite(fi) && isfinite(f1)");
    volatile uint64_t n1 = 0xfedcba9876543210ull, d1 = 12345, d2 = 0x123456789ull, d3 = 0x8000000000000001ull;
    CHECK(n1 / d1 == ref_udiv(n1, d1) && n1 % d1 == n1 - ref_udiv(n1, d1) * d1, "%s", "n1 / d1 == ref_udiv(n1, d1) && n1 % d1 == n1 - ref_udiv(n1, d1) * d1");
    CHECK(n1 / d2 == ref_udiv(n1, d2) && n1 % d2 == n1 - ref_udiv(n1, d2) * d2, "%s", "n1 / d2 == ref_udiv(n1, d2) && n1 % d2 == n1 - ref_udiv(n1, d2) * d2");
    CHECK(n1 / d3 == 1 && n1 % d3 == n1 - d3, "%s", "n1 / d3 == 1 && n1 % d3 == n1 - d3");
    volatile int64_t s1 = -1000000000007ll, s2 = 7, s3 = -3;
    CHECK(s1 / s2 == -142857142858ll && s1 % s2 == -1, "%s", "s1 / s2 == -142857142858ll && s1 % s2 == -1");
    CHECK(s1 / s3 == 333333333335ll && s1 % s3 == -2, "%s", "s1 / s3 == 333333333335ll && s1 % s3 == -2");
    // call cost
    const int N = 2000000;
    double t0 = now();
    volatile int sink = 0;
    for (int i = 0; i < N; i++) { vn = 8; sink += memcmp(a, b, vn); }
    double t1 = now();
    printf("memcmp(8): %.1f ns/call\n", (t1 - t0) / N * 1e9);
    DONE();
}
