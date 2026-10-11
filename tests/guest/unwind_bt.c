// Unwinder self-test: walk the stack with _Unwind_Backtrace.
#include <stdint.h>
#include <stdio.h>
#include <unwind.h>
static int depth;
static _Unwind_Reason_Code cb(struct _Unwind_Context *ctx, void *arg) {
    printf("  frame %d: ip=%#lx\n", depth++, (unsigned long)_Unwind_GetIP(ctx));
    return depth > 10 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
__attribute__((noinline)) static void level3(void) { _Unwind_Backtrace(cb, 0); }
__attribute__((noinline)) static void level2(void) { level3(); __asm__ volatile(""); }
__attribute__((noinline)) static void level1(void) { level2(); __asm__ volatile(""); }
int main(void) {
    printf("main=%p level1=%p level2=%p level3=%p\n", (void *)main, (void *)level1, (void *)level2, (void *)level3);
    level1();
    return 0;
}
