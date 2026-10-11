// Basic libc smoke test for the guest runtime.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int counter = 5;
static void bye(void) { printf("atexit handler ran (counter=%d)\n", counter); }

int main(int argc, char **argv) {
    printf("hello from i386 guest: argc=%d argv[0]=%s\n", argc, argv[0]);
    char *p = malloc(64);
    strcpy(p, "Deus Ex");
    strcat(p, ": Human Revolution");
    printf("%s (len %zu) %5.2f %lld %x %c|%-6s|\n", p, strlen(p), 3.14159, 1234567890123LL, 0xbeef, 'Z', "ab");
    int a = 0; float f = 0; char word[32];
    int n = sscanf("42 2.5 adam", "%d %f %31s", &a, &f, word);
    printf("sscanf -> %d: %d %.2f %s\n", n, a, f, word);
    long long big = 1LL << 40;
    printf("64-bit division: %lld\n", big / 3);
    char buf[16];
    snprintf(buf, sizeof buf, "%s", "truncate-me-please");
    printf("snprintf: %s\n", buf);
    free(p);
    atexit(bye);
    counter = 7;
    return 3;
}
