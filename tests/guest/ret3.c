#include <stdlib.h>
__attribute__((naked)) static void h1(void) { __asm__("ret"); }
__attribute__((naked)) static void h2(void) { __asm__("ret"); }
__attribute__((naked)) static void h3(void) { __asm__("ret"); }
__attribute__((naked)) static void p1(void) { __asm__("call 1f\n1: pop %eax\nret"); }
__attribute__((naked)) static void p2(void) { __asm__("call 1f\n1: pop %eax\nret"); }
int main(void) {
    if (getenv("PIC")) { atexit(p1); atexit(p2); } else { atexit(h1); atexit(h2); atexit(h3); }
    return 0;
}
