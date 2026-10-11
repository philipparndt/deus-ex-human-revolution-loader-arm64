// OSAtomic/OSSpinLock, keymgr, regex, sched.
#include <libkern/OSAtomic.h>
#include <regex.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include "libc_check.h"

extern void *_keymgr_get_and_lock_processwide_ptr(unsigned int key);
extern int _keymgr_get_and_lock_processwide_ptr_2(unsigned int key, void **result);
extern int _keymgr_set_and_unlock_processwide_ptr(unsigned int key, void *ptr);

_Static_assert(sizeof(regex_t) == 16 && sizeof(regmatch_t) == 16, "regex");

int main(void) {
    volatile int32_t v = 5;
    CHECK(OSAtomicAdd32(3, &v) == 8 && v == 8, "OSAtomicAdd32");
    CHECK(OSAtomicAdd32Barrier(-10, &v) == -2 && v == -2, "OSAtomicAdd32Barrier");
    CHECK(OSAtomicIncrement32(&v) == -1 && OSAtomicDecrement32Barrier(&v) == -2, "inc/dec");
    volatile int64_t v64 __attribute__((aligned(8))) = 0x100000000LL;
    CHECK(OSAtomicAdd64(0x200000001LL, &v64) == 0x300000001LL && v64 == 0x300000001LL, "OSAtomicAdd64");
    int ci = 7;
    CHECK(OSAtomicCompareAndSwapInt(7, 9, &ci) && ci == 9 && !OSAtomicCompareAndSwapInt(7, 11, &ci) && ci == 9, "CASInt");
    void *p = NULL;
    int dummy;
    CHECK(OSAtomicCompareAndSwapPtrBarrier(NULL, &dummy, &p) && p == &dummy && !OSAtomicCompareAndSwapPtrBarrier(NULL, (void *)&ci, &p), "CASPtr");
    volatile int64_t c64 __attribute__((aligned(8))) = 1;
    CHECK(OSAtomicCompareAndSwap64(1, 0x123456789LL, &c64) && c64 == 0x123456789LL, "CAS64");
    volatile uint32_t bits = 0xF0;
    CHECK(OSAtomicOr32Orig(0x0F, &bits) == 0xF0 && bits == 0xFF && OSAtomicAnd32(0x3C, &bits) == 0x3C, "or/and");
    uint8_t tas[2] = {0, 0};
    CHECK(OSAtomicTestAndSet(0, tas) == 0 && tas[0] == 0x80 && OSAtomicTestAndSet(0, tas) == 1 && OSAtomicTestAndSet(9, tas) == 0 && tas[1] == 0x40, "TestAndSet");
    OSMemoryBarrier();
    OSSpinLock lock = OS_SPINLOCK_INIT;
    OSSpinLockLock(&lock);
    CHECK(lock != 0 && !OSSpinLockTry(&lock), "spinlock held");
    OSSpinLockUnlock(&lock);
    CHECK(lock == 0 && OSSpinLockTry(&lock), "spinlock released");
    OSSpinLockUnlock(&lock);
    OSQueueHead q = OS_ATOMIC_QUEUE_INIT;
    struct node { int val; struct node *next; } n1 = {1, 0}, n2 = {2, 0};
    OSAtomicEnqueue(&q, &n1, offsetof(struct node, next));
    OSAtomicEnqueue(&q, &n2, offsetof(struct node, next));
    struct node *d1 = OSAtomicDequeue(&q, offsetof(struct node, next)), *d2 = OSAtomicDequeue(&q, offsetof(struct node, next));
    CHECK(d1 == &n2 && d2 == &n1 && OSAtomicDequeue(&q, offsetof(struct node, next)) == NULL, "OSAtomic queue");

    CHECK(_keymgr_get_and_lock_processwide_ptr(5) == NULL, "keymgr initial");
    CHECK(_keymgr_set_and_unlock_processwide_ptr(5, &dummy) == 0, "keymgr set");
    void *got = NULL;
    CHECK(_keymgr_get_and_lock_processwide_ptr_2(5, &got) == 0 && got == &dummy, "keymgr get_2");
    CHECK(_keymgr_get_and_lock_processwide_ptr(5) == &dummy, "keymgr recursive lock");
    _keymgr_set_and_unlock_processwide_ptr(5, &dummy);
    _keymgr_set_and_unlock_processwide_ptr(5, &dummy);

    regex_t re;
    CHECK(regcomp(&re, "([a-z]+)-([0-9]+)", REG_EXTENDED) == 0 && re.re_nsub == 2, "regcomp");
    regmatch_t m[3];
    CHECK(regexec(&re, "id: abc-123!", 3, m, 0) == 0 && m[0].rm_so == 4 && m[0].rm_eo == 11 && m[1].rm_so == 4 &&
              m[1].rm_eo == 7 && m[2].rm_so == 8 && m[2].rm_eo == 11,
          "regexec");
    CHECK(regexec(&re, "nothing", 0, NULL, 0) == REG_NOMATCH, "regexec nomatch");
    regfree(&re);
    char eb[64];
    CHECK(regcomp(&re, "(unbalanced", REG_EXTENDED) != 0, "regcomp error");
    CHECK(regerror(REG_EPAREN, NULL, eb, sizeof eb) > 0 && eb[0], "regerror");

    CHECK(sched_yield() == 0, "sched_yield");
    CHECK(sched_get_priority_min(SCHED_OTHER) <= sched_get_priority_max(SCHED_OTHER), "sched prio");
    DONE();
}
