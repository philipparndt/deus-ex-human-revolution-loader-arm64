// time, gettimeofday, struct tm conversions, strftime, ftime, mach time.
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <time.h>
#include <unistd.h>
#include "libc_check.h"

_Static_assert(sizeof(struct tm) == 44 && offsetof(struct tm, tm_zone) == 40, "tm");
_Static_assert(sizeof(struct timeb) == 12, "timeb");

int main(void) {
    time_t t0 = time(NULL), t1;
    CHECK(t0 > 1700000000 && time(&t1) >= t0 && t1 >= t0, "time %ld", (long)t0);
    struct timeval tv;
    struct timezone tz;
    CHECK(gettimeofday(&tv, &tz) == 0 && tv.tv_sec >= t0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000, "gettimeofday");
    CHECK(difftime(t0 + 90, t0) == 90.0, "difftime");
    volatile clock_t c = clock();
    CHECK(c >= 0, "clock");

    time_t fixed = 1234567890; // 2009-02-13 23:31:30 UTC
    struct tm *g = gmtime(&fixed);
    CHECK(g && g->tm_year == 109 && g->tm_mon == 1 && g->tm_mday == 13 && g->tm_hour == 23 && g->tm_min == 31 &&
              g->tm_sec == 30 && g->tm_wday == 5 && g->tm_yday == 43 && g->tm_gmtoff == 0,
          "gmtime");
    CHECK(g && g->tm_zone && strcmp(g->tm_zone, "UTC") == 0, "gmtime zone %s", g && g->tm_zone ? g->tm_zone : "(null)");
    struct tm r;
    memset(&r, 0, sizeof r);
    CHECK(gmtime_r(&fixed, &r) == &r && r.tm_mday == 13, "gmtime_r");
    char buf[128];
    CHECK(strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z %a %j", &r) > 0 && strcmp(buf, "2009-02-13 23:31:30 UTC Fri 044") == 0,
          "strftime '%s'", buf);
    struct tm *l = localtime(&fixed);
    CHECK(l && l->tm_zone != NULL, "localtime");
    struct tm lr;
    CHECK(localtime_r(&fixed, &lr) == &lr && lr.tm_hour == l->tm_hour && lr.tm_gmtoff == l->tm_gmtoff, "localtime_r");
    time_t back = mktime(&lr);
    CHECK(back == fixed, "mktime %ld", (long)back);
    // mktime normalises fields in place
    struct tm n = lr;
    n.tm_mday += 31;
    n.tm_isdst = -1;
    time_t nt = mktime(&n);
    CHECK(nt > fixed && n.tm_mon == (lr.tm_mon + 1) % 12 && n.tm_mday == lr.tm_mday + 31 - 28, "mktime normalise %d/%d", n.tm_mon, n.tm_mday);
    CHECK(timegm(&r) == fixed, "timegm");
    CHECK(strcmp(asctime(&r), "Fri Feb 13 23:31:30 2009\n") == 0, "asctime");

    struct timeb tb;
    CHECK(ftime(&tb) == 0 && tb.time >= t0 && tb.millitm < 1000, "ftime");
    CHECK(tb.timezone == (short)(-lr.tm_gmtoff / 60 + (lr.tm_isdst > 0 ? 60 : 0)), "ftime timezone %d", tb.timezone);

    mach_timebase_info_data_t tbi = {0, 0};
    CHECK(mach_timebase_info(&tbi) == 0 && tbi.numer > 0 && tbi.denom > 0, "mach_timebase_info");
    CHECK(tbi.numer == 1 && tbi.denom == 1, "Intel timebase 1/1 (got %u/%u)", tbi.numer, tbi.denom);
    uint64_t a = mach_absolute_time();
    struct timespec req = {0, 20 * 1000 * 1000}, rem;
    CHECK(nanosleep(&req, &rem) == 0, "nanosleep");
    uint64_t b = mach_absolute_time();
    uint64_t ns = (b - a) * tbi.numer / tbi.denom;
    CHECK(ns >= 19000000ULL && ns < 2000000000ULL, "mach_absolute_time delta %llu", (unsigned long long)ns);
    DONE();
}
