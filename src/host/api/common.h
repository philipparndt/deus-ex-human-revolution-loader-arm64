// Shared helpers for API thunks.
#pragma once
#include "../hobj.h"
#include "../thunk.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

// --- errno -----------------------------------------------------------------
// Guest address of the calling thread's errno variable.
gaddr_t guest_errno_addr(void);
static inline void set_gerrno(int e) { gwrite32(guest_errno_addr(), (uint32_t)e); }
// Return a POSIX-style result; on -1 copy the host errno to the guest.
#define RET_ERRNO(expr)                                                                            \
    do {                                                                                           \
        long ret_ = (long)(expr);                                                                  \
        if (ret_ == -1) set_gerrno(errno);                                                         \
        RET(ret_);                                                                                 \
    } while (0)

// --- guest varargs (i386 va_list is a pointer into the argument area) --------
struct gva {
    gaddr_t p;
};
static inline struct gva gva_at(gaddr_t p) { return (struct gva){p}; }
// va_list for the variadic arguments that start at argument index i.
#define GVA_FROM_ARG(i) gva_at(r->esp + 4 + 4 * (i))
static inline uint32_t gva_u32(struct gva *va) {
    uint32_t v = gread32(va->p);
    va->p += 4;
    return v;
}
static inline uint64_t gva_u64(struct gva *va) {
    uint64_t v = gread32(va->p) | ((uint64_t)gread32(va->p + 4) << 32);
    va->p += 8;
    return v;
}
static inline double gva_double(struct gva *va) {
    uint64_t v = gva_u64(va);
    double d;
    memcpy(&d, &v, 8);
    return d;
}
// 80-bit x87 long double, passed as 16 bytes on Darwin i386.
double x87_to_double(const uint8_t *p);
void double_to_x87(double d, uint8_t *p);
static inline double gva_longdouble(struct gva *va) {
    double d = x87_to_double(g2h(va->p));
    va->p += 16;
    return d;
}

// printf-style formatting reading arguments from guest memory. Behaves like
// vsnprintf (returns the full length, writes at most cap bytes incl. NUL).
int gfmt_vsnprintf(char *out, size_t cap, const char *fmt, struct gva va);
// Same, but allocates the result with malloc (host). Returns length or -1.
int gfmt_vasprintf(char **out, const char *fmt, struct gva va);
// scanf-style parsing writing results to guest memory; returns like vsscanf.
int gfmt_vsscanf(const char *str, const char *fmt, struct gva va);

// --- strings -----------------------------------------------------------------
// Copy a host string into a guest buffer of size cap (always NUL-terminated if cap > 0).
static inline void gstrlcpy(gaddr_t dst, const char *src, uint32_t cap) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(g2h(dst), src, n);
    gwrite8(dst + (uint32_t)n, 0);
}

// A per-call scratch guest string kept alive per thread for "returns pointer to
// static buffer" APIs (strerror, getenv results are handled separately).
gaddr_t guest_static_string(int slot, const char *s);

// --- time structures (i386 layouts) -------------------------------------------
struct timeval;
struct timespec;
struct tm;
void put_gtimeval(gaddr_t g, const struct timeval *tv);
void get_gtimeval(gaddr_t g, struct timeval *tv);
void put_gtimespec(gaddr_t g, const struct timespec *ts);
void get_gtimespec(gaddr_t g, struct timespec *ts);
void put_gtm(gaddr_t g, const struct tm *tm);
void get_gtm(gaddr_t g, struct tm *tm);
