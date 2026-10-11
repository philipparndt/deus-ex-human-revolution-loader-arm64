// <time.h>, <sys/time.h>, <sys/timeb.h> and the mach time base.
//
// i386: time_t, clock_t and long are 32-bit; struct tm is 44 bytes (see
// common.c); struct timeb is {time_t time; u16 millitm; i16 timezone; i16 dstflag} (12 bytes).
#include "common.h"
#include "libc.h"

#include <mach/mach_time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>

// --- struct tm with stable tm_zone strings ------------------------------------------------
void libc_put_tm(gaddr_t g, const struct tm *tm) {
    const int *f = &tm->tm_sec;
    for (int i = 0; i < 9; i++) gwrite32(g + 4 * i, (uint32_t)f[i]);
    gwrite32(g + 36, (uint32_t)tm->tm_gmtoff);
    gwrite32(g + 40, gintern(tm->tm_zone));
}
void libc_get_tm(gaddr_t g, struct tm *tm) {
    get_gtm(g, tm);
    gaddr_t z = gread32(g + 40);
    tm->tm_zone = z ? (char *)g2h(z) : NULL;
}

static time_t gtime_at(gaddr_t p) { return (time_t)(int32_t)gread32(p); }

// Result of mktime-like calls that may not fit a 32-bit time_t.
static uint32_t to_gtime(time_t t) {
    if (t != (time_t)(int32_t)t) {
        set_gerrno(EOVERFLOW);
        return (uint32_t)-1;
    }
    return (uint32_t)t;
}

THUNK(time) {
    time_t now = time(NULL);
    if (ARG(0)) gwrite32(ARG(0), (uint32_t)now);
    RET((uint32_t)now);
}
THUNK(gettimeofday) {
    struct timeval tv;
    struct timezone tz;
    int rc = gettimeofday(&tv, ARG(1) ? &tz : NULL);
    if (rc) {
        set_gerrno(errno);
        RET(rc);
        return;
    }
    if (ARG(0)) put_gtimeval(ARG(0), &tv);
    if (ARG(1)) memcpy(ARGP(1), &tz, 8); // struct timezone {int, int}
    RET(0);
}
THUNK(clock) { RET((uint32_t)clock()); }
THUNK_ALIAS(clock_unix2003, "_clock$UNIX2003", clock)
THUNK(difftime) { RETD(difftime((time_t)ARGI(0), (time_t)ARGI(1))); }

THUNK(ftime) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm lt;
    time_t t = tv.tv_sec;
    localtime_r(&t, &lt);
    gaddr_t g = ARG(0);
    gwrite32(g, (uint32_t)tv.tv_sec);
    gwrite16(g + 4, (uint16_t)(tv.tv_usec / 1000));
    gwrite16(g + 6, (uint16_t)(int16_t)(-lt.tm_gmtoff / 60 + (lt.tm_isdst > 0 ? 60 : 0)));
    gwrite16(g + 8, (uint16_t)(lt.tm_isdst > 0));
    RET(0);
}

// --- broken-down time -------------------------------------------------------------------------
static _Thread_local gaddr_t tls_localtm, tls_gmtm;

static gaddr_t tm_buffer(gaddr_t *slot) {
    if (!*slot) *slot = gheap_calloc(1, 44);
    return *slot;
}

THUNK(localtime) {
    time_t t = gtime_at(ARG(0));
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        RET(0);
        return;
    }
    gaddr_t g = tm_buffer(&tls_localtm);
    libc_put_tm(g, &tm);
    RET(g);
}
THUNK(gmtime) {
    time_t t = gtime_at(ARG(0));
    struct tm tm;
    if (!gmtime_r(&t, &tm)) {
        RET(0);
        return;
    }
    gaddr_t g = tm_buffer(&tls_gmtm);
    libc_put_tm(g, &tm);
    RET(g);
}
THUNK(localtime_r) {
    time_t t = gtime_at(ARG(0));
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        RET(0);
        return;
    }
    libc_put_tm(ARG(1), &tm);
    RET(ARG(1));
}
THUNK(gmtime_r) {
    time_t t = gtime_at(ARG(0));
    struct tm tm;
    if (!gmtime_r(&t, &tm)) {
        RET(0);
        return;
    }
    libc_put_tm(ARG(1), &tm);
    RET(ARG(1));
}
// mktime normalises the caller's struct tm in place.
THUNK(mktime) {
    struct tm tm;
    libc_get_tm(ARG(0), &tm);
    tm.tm_zone = NULL;
    time_t t = mktime(&tm);
    if (t != -1) libc_put_tm(ARG(0), &tm);
    RET(t == -1 ? (uint32_t)-1 : to_gtime(t));
}
THUNK_ALIAS(timelocal, "_timelocal", mktime)
THUNK_ALIAS(mktime_unix2003, "_mktime$UNIX2003", mktime)
THUNK(timegm) {
    struct tm tm;
    libc_get_tm(ARG(0), &tm);
    tm.tm_zone = NULL;
    time_t t = timegm(&tm);
    if (t != -1) libc_put_tm(ARG(0), &tm);
    RET(t == -1 ? (uint32_t)-1 : to_gtime(t));
}
THUNK(tzset) { tzset(); }

THUNK(asctime) {
    struct tm tm;
    char buf[64];
    libc_get_tm(ARG(0), &tm);
    RET(asctime_r(&tm, buf) ? guest_static_string(3, buf) : 0);
}
THUNK(asctime_r) {
    struct tm tm;
    char buf[64];
    libc_get_tm(ARG(0), &tm);
    if (!asctime_r(&tm, buf)) {
        RET(0);
        return;
    }
    gstrlcpy(ARG(1), buf, 26);
    RET(ARG(1));
}
THUNK(ctime) {
    time_t t = gtime_at(ARG(0));
    char buf[64];
    RET(ctime_r(&t, buf) ? guest_static_string(3, buf) : 0);
}
THUNK(ctime_r) {
    time_t t = gtime_at(ARG(0));
    char buf[64];
    if (!ctime_r(&t, buf)) {
        RET(0);
        return;
    }
    gstrlcpy(ARG(1), buf, 26);
    RET(ARG(1));
}

// strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
THUNK(strftime) {
    struct tm tm;
    libc_get_tm(ARG(3), &tm);
    RET(strftime(ARGP(0), ARG(1), ARGS(2), &tm));
}
THUNK_ALIAS(strftime_unix2003, "_strftime$UNIX2003", strftime)
// wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *tm); wchar_t is 32-bit on both sides.
THUNK(wcsftime) {
    struct tm tm;
    libc_get_tm(ARG(3), &tm);
    RET(wcsftime(ARGP(0), ARG(1), ARGP(2), &tm));
}
THUNK_ALIAS(wcsftime_unix2003, "_wcsftime$UNIX2003", wcsftime)
THUNK(strptime) {
    struct tm tm;
    libc_get_tm(ARG(2), &tm);
    char *end = strptime(ARGS(0), ARGS(1), &tm);
    libc_put_tm(ARG(2), &tm);
    RET(end ? ARG(0) + (uint32_t)(end - ARGS(0)) : 0);
}
THUNK_ALIAS(strptime_unix2003, "_strptime$UNIX2003", strptime)

// --- sleeping and clocks ------------------------------------------------------------------------
THUNK(nanosleep) {
    struct timespec req, rem;
    get_gtimespec(ARG(0), &req);
    int rc = nanosleep(&req, &rem);
    if (rc) {
        set_gerrno(errno);
        if (ARG(1)) put_gtimespec(ARG(1), &rem);
    }
    RET(rc);
}
THUNK_ALIAS(nanosleep_unix2003, "_nanosleep$UNIX2003", nanosleep)
THUNK_ALIAS(nanosleep_nocancel, "_nanosleep$NOCANCEL$UNIX2003", nanosleep)

// clockid_t values are the same on both sides.
THUNK(clock_gettime) {
    struct timespec ts;
    int rc = clock_gettime((clockid_t)ARGI(0), &ts);
    if (rc) set_gerrno(errno);
    else put_gtimespec(ARG(1), &ts);
    RET(rc);
}
THUNK(clock_getres) {
    struct timespec ts;
    int rc = clock_getres((clockid_t)ARGI(0), &ts);
    if (rc) set_gerrno(errno);
    else if (ARG(1)) put_gtimespec(ARG(1), &ts);
    RET(rc);
}

// The guest sees Intel Mac absolute time: 1 tick = 1 ns, timebase 1/1. Games of
// that era assume it (arm64 hosts tick at 24 MHz, timebase 125/3), and Rosetta
// presents the same to x86 code.
uint64_t guest_abs_time(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t guest_abs_to_host(uint64_t ns) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (uint64_t)((__uint128_t)ns * tb.denom / tb.numer);
}
THUNK(mach_absolute_time) { RET64(guest_abs_time()); }
THUNK(mach_timebase_info) {
    gwrite32(ARG(0), 1);
    gwrite32(ARG(0) + 4, 1);
    RET(KERN_SUCCESS);
}
THUNK(mach_wait_until) { RET(mach_wait_until(guest_abs_to_host(ARG64(0)))); }
