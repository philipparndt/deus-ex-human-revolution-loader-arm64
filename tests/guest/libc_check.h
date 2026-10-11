// Tiny check helper for libc guest tests.
#include <stdio.h>
static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) g_pass++;                                                                        \
        else {                                                                                     \
            g_fail++;                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)
#define DONE()                                                                                     \
    do {                                                                                           \
        printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);            \
        return g_fail != 0;                                                                        \
    } while (0)
