#include <stdio.h>
#include <wchar.h>
int main(void) {
    char b[64];
    void *p = (void *)0x32123938;
    snprintf(b, sizeof b, "[%018p]", p); printf("%s\n", b);
    snprintf(b, sizeof b, "[%p] [%20p] [%-12p|]", p, p, p); printf("%s\n", b);
    void *q = 0; sscanf("FERAL_WSTR||0x0000000032123938", "FERAL_WSTR||%p", &q); printf("scan %p\n", q);
    return 0;
}
