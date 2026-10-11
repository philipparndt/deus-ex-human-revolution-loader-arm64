#include <stdlib.h>
static volatile int x;
static void h1(void) { x++; }
static void h2(void) { x++; }
static void h3(void) { x++; }
int main(void) { atexit(h1); atexit(h2); atexit(h3); return 0; }
