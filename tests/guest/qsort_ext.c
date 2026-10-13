// qsort: sorted result, permutation kept, and the comparator only ever sees
// pointers into the array (libc's heapsort fallback compared against a copy
// outside guest memory and crashed HR), over many sizes and input patterns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct rec {
    int key;
    int id; // original index: checks the permutation
    char pad[4];
};

static const char *lo_ptr, *hi_ptr;
static size_t elem;
static long calls, outside;

static void in_array(const void *p) {
    const char *c = p;
    if (c < lo_ptr || c >= hi_ptr || (size_t)(c - lo_ptr) % elem) outside++;
}
static int cmp_int(const void *a, const void *b) {
    in_array(a), in_array(b), calls++;
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}
static int cmp_rec(const void *a, const void *b) {
    in_array(a), in_array(b), calls++;
    int x = ((const struct rec *)a)->key, y = ((const struct rec *)b)->key;
    return x < y ? -1 : x > y;
}

static unsigned rng = 12345;
static unsigned rnd(void) { return rng = rng * 1103515245u + 12345u, rng >> 8; }

static int gen(int pattern, int i, int n) {
    switch (pattern) {
    case 0: return (int)rnd();
    case 1: return i;
    case 2: return n - i;
    case 3: return 7;
    case 4: return (int)(rnd() % 4);
    case 5: return i < n / 2 ? i : n - i; // organ pipe
    case 6: return i % 2 ? i : -i;
    default: return (int)(rnd() % 16) - (i % 5 ? 0 : 1000);
    }
}

int main(void) {
    static const int sizes[] = {0, 1, 2, 3, 5, 16, 17, 31, 64, 100, 257, 1000, 4096, 20000};
    for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        int n = sizes[si];
        for (int pat = 0; pat < 8; pat++) {
            // ints
            int *v = malloc((n ? n : 1) * sizeof *v);
            long sum = 0;
            for (int i = 0; i < n; i++) sum += v[i] = gen(pat, i, n);
            lo_ptr = (const char *)v, hi_ptr = (const char *)(v + n), elem = sizeof *v;
            outside = 0;
            qsort(v, n, sizeof *v, cmp_int);
            long sum2 = 0;
            int sorted = 1;
            for (int i = 0; i < n; i++) {
                sum2 += v[i];
                if (i && v[i - 1] > v[i]) sorted = 0;
            }
            CHECK(sorted && sum == sum2 && !outside, "int n=%d pattern %d sorted=%d sum ok=%d outside=%ld", n, pat,
                  sorted, sum == sum2, outside);
            free(v);

            // 12-byte records
            struct rec *r = malloc((n ? n : 1) * sizeof *r);
            for (int i = 0; i < n; i++) r[i] = (struct rec){gen(pat, i, n), i, {0}};
            lo_ptr = (const char *)r, hi_ptr = (const char *)(r + n), elem = sizeof *r;
            outside = 0;
            qsort(r, n, sizeof *r, cmp_rec);
            char *seen = calloc(n ? n : 1, 1);
            int ok = 1;
            for (int i = 0; i < n; i++) {
                if (i && r[i - 1].key > r[i].key) ok = 0;
                if (r[i].id < 0 || r[i].id >= n || seen[r[i].id]++) ok = 0;
            }
            CHECK(ok && !outside, "rec n=%d pattern %d ok=%d outside=%ld", n, pat, ok, outside);
            free(seen);
            free(r);
        }
    }
    printf("qsort_ext: %s (%d failures, %ld compares)\n", failures ? "FAILED" : "OK", failures, calls);
    return failures != 0;
}
