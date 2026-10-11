#include <stdio.h>
#include <stdlib.h>
static int calls;
static int cmp(const void *a, const void *b) { calls++; return *(const int *)a - *(const int *)b; }
int main(int argc, char **argv) {
    int n = atoi(getenv("N") ? getenv("N") : "2");
    int v[8] = {5, 3, 7, 1, 2, 8, 6, 4};
    puts("before qsort");
    qsort(v, n, sizeof v[0], cmp);
    printf("after qsort: calls=%d first=%d\n", calls, v[0]);
    return 0;
}
