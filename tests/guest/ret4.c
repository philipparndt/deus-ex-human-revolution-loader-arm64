#include <stdlib.h>
int counter; // ends up on the same page as the code
__attribute__((naked)) static void w1(void) { __asm__("call 1f\n1: pop %eax\nincl _counter-1b(%eax)\nret"); }
__attribute__((naked)) static void w2(void) { __asm__("call 1f\n1: pop %eax\nincl _counter-1b(%eax)\nret"); }
int main(void) { atexit(w1); atexit(w2); return 0; }
