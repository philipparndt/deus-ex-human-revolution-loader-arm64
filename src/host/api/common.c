#include "common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

// --- errno -----------------------------------------------------------------

static _Thread_local gaddr_t errno_slot;

gaddr_t guest_errno_addr(void) {
    if (!errno_slot) errno_slot = gheap_calloc(1, 16);
    return errno_slot;
}

THUNK(__error) { RET(guest_errno_addr()); }

// --- static strings ------------------------------------------------------------

#define NSTATIC 8
static _Thread_local struct { gaddr_t buf; uint32_t cap; } static_strings[NSTATIC];

gaddr_t guest_static_string(int slot, const char *s) {
    uint32_t need = (uint32_t)strlen(s) + 1;
    if (static_strings[slot].cap < need) {
        gheap_free(static_strings[slot].buf);
        static_strings[slot].cap = need < 256 ? 256 : need;
        static_strings[slot].buf = gheap_alloc(static_strings[slot].cap);
    }
    memcpy(g2h(static_strings[slot].buf), s, need);
    return static_strings[slot].buf;
}

// --- x87 extended precision -------------------------------------------------------

double x87_to_double(const uint8_t *p) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double d = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -d : d;
}

void double_to_x87(double d, uint8_t *p) {
    uint16_t se = signbit(d) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (isnan(d)) {
        se |= 0x7fff;
        mant = 0xC000000000000000ULL;
    } else if (isinf(d)) {
        se |= 0x7fff;
        mant = 0x8000000000000000ULL;
    } else if (d != 0) {
        int e;
        double m = frexp(fabs(d), &e); // m in [0.5, 1)
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
    memset(p + 10, 0, 6);
}

// --- time structures ---------------------------------------------------------------

void put_gtimeval(gaddr_t g, const struct timeval *tv) {
    gwrite32(g, (uint32_t)tv->tv_sec);
    gwrite32(g + 4, (uint32_t)tv->tv_usec);
}
void get_gtimeval(gaddr_t g, struct timeval *tv) {
    tv->tv_sec = (int32_t)gread32(g);
    tv->tv_usec = (int32_t)gread32(g + 4);
}
void put_gtimespec(gaddr_t g, const struct timespec *ts) {
    gwrite32(g, (uint32_t)ts->tv_sec);
    gwrite32(g + 4, (uint32_t)ts->tv_nsec);
}
void get_gtimespec(gaddr_t g, struct timespec *ts) {
    ts->tv_sec = (int32_t)gread32(g);
    ts->tv_nsec = (int32_t)gread32(g + 4);
}

// i386 struct tm: 9 ints, long tm_gmtoff, char *tm_zone (44 bytes)
void put_gtm(gaddr_t g, const struct tm *tm) {
    const int *f = &tm->tm_sec;
    for (int i = 0; i < 9; i++) gwrite32(g + 4 * i, (uint32_t)f[i]);
    gwrite32(g + 36, (uint32_t)tm->tm_gmtoff);
    gwrite32(g + 40, tm->tm_zone ? guest_static_string(NSTATIC - 1, tm->tm_zone) : 0);
}
void get_gtm(gaddr_t g, struct tm *tm) {
    memset(tm, 0, sizeof *tm);
    int *f = &tm->tm_sec;
    for (int i = 0; i < 9; i++) f[i] = (int32_t)gread32(g + 4 * i);
    tm->tm_gmtoff = (int32_t)gread32(g + 36);
}

// --- printf ---------------------------------------------------------------------

struct outbuf {
    char *buf;
    size_t cap, len;
};

static void out_append(struct outbuf *o, const char *s, size_t n) {
    if (o->len < o->cap) {
        size_t room = o->cap - o->len;
        memcpy(o->buf + o->len, s, n < room ? n : room);
    }
    o->len += n;
}

int gfmt_vsnprintf(char *out, size_t cap, const char *fmt, struct gva va) {
    struct outbuf o = {out, cap ? cap - 1 : 0, 0};
    char spec[64], tmp[512];
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            const char *q = strchr(p, '%');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            out_append(&o, p, n);
            p += n;
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            out_append(&o, "%", 1);
            p++;
            continue;
        }
        // Parse flags, width, precision, length; build a host format spec.
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ #0'", *p)) spec[sl++] = *p++;
        int width = -1, prec = -1;
        bool have_width_star = false, have_prec_star = false;
        if (*p == '*') {
            width = (int32_t)gva_u32(&va);
            have_width_star = true;
            p++;
        } else
            while (*p >= '0' && *p <= '9') spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') {
                prec = (int32_t)gva_u32(&va);
                have_prec_star = true;
                p++;
            } else
                while (*p >= '0' && *p <= '9') spec[sl++] = *p++;
        }
        // Length modifier (guest sizes: int/long/pointer/size_t = 4 bytes).
        int lmod = 0; // 0 none, 1 hh, 2 h, 3 l, 4 ll/q/j, 5 z/t, 6 L
        if (*p == 'h') { lmod = 2; p++; if (*p == 'h') { lmod = 1; p++; } }
        else if (*p == 'l') { lmod = 3; p++; if (*p == 'l') { lmod = 4; p++; } }
        else if (*p == 'q' || *p == 'j') { lmod = 4; p++; }
        else if (*p == 'z' || *p == 't') { lmod = 5; p++; }
        else if (*p == 'L') { lmod = 6; p++; }
        char conv = *p ? *p++ : 0;
        if (!conv) break;
        spec[sl] = 0;
        // Rebuild spec with explicit numbers for star width/precision.
        char final_spec[96];
        {
            const char *f = spec + 1;
            size_t fl = 0;
            while (f[fl] && strchr("-+ #0'", f[fl])) fl++;
            int n = snprintf(final_spec, sizeof final_spec, "%%%.*s", (int)fl, f);
            const char *rest = f + fl;
            if (have_width_star) {
                if (width < 0) { n += snprintf(final_spec + n, sizeof final_spec - n, "-"); width = -width; }
                n += snprintf(final_spec + n, sizeof final_spec - n, "%d", width);
                // rest starts with '.' or end (no digits were consumed)
            } else {
                while (*rest >= '0' && *rest <= '9') final_spec[n++] = *rest++;
            }
            if (*rest == '.') {
                rest++;
                if (have_prec_star) {
                    if (prec >= 0) n += snprintf(final_spec + n, sizeof final_spec - n, ".%d", prec);
                } else {
                    final_spec[n++] = '.';
                    while (*rest >= '0' && *rest <= '9') final_spec[n++] = *rest++;
                }
            }
            final_spec[n] = 0;
        }
        size_t fl = strlen(final_spec);
        int len = 0;
        switch (conv) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
            final_spec[fl] = 'l';
            final_spec[fl + 1] = 'l';
            final_spec[fl + 2] = conv;
            final_spec[fl + 3] = 0;
            bool is_signed = conv == 'd' || conv == 'i';
            long long v;
            if (lmod == 4) v = (long long)gva_u64(&va);
            else {
                uint32_t w = gva_u32(&va);
                if (lmod == 1) v = is_signed ? (long long)(int8_t)w : (long long)(uint8_t)w;
                else if (lmod == 2) v = is_signed ? (long long)(int16_t)w : (long long)(uint16_t)w;
                else v = is_signed ? (long long)(int32_t)w : (long long)w;
            }
            len = snprintf(tmp, sizeof tmp, final_spec, v);
            break;
        }
        case 'c': {
            final_spec[fl] = 'c';
            final_spec[fl + 1] = 0;
            len = snprintf(tmp, sizeof tmp, final_spec, (int)gva_u32(&va));
            break;
        }
        case 'C': lmod = 3; /* fallthrough */
        case 's': case 'S': {
            gaddr_t s = gva_u32(&va);
            if (conv == 'S' || lmod == 3) {
                // wide string: wchar_t is 32-bit on both sides
                final_spec[fl] = 'l';
                final_spec[fl + 1] = 's';
                final_spec[fl + 2] = 0;
                char *dyn = NULL;
                len = asprintf(&dyn, final_spec, s ? (const wchar_t *)g2h(s) : L"(null)");
                if (len > 0) out_append(&o, dyn, (size_t)len);
                free(dyn);
                continue;
            }
            final_spec[fl] = 's';
            final_spec[fl + 1] = 0;
            char *dyn = NULL;
            len = asprintf(&dyn, final_spec, s ? (const char *)g2h(s) : "(null)");
            if (len > 0) out_append(&o, dyn, (size_t)len);
            free(dyn);
            continue;
        }
        case 'p': {
            // host %p gives Darwin's formatting incl. flags/width ("%018p" ->
            // 0x0000000032123938, which Feral's FERAL_WSTR|| strings rely on)
            final_spec[fl] = 'p';
            final_spec[fl + 1] = 0;
            len = snprintf(tmp, sizeof tmp, final_spec, (void *)(uintptr_t)gva_u32(&va));
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            final_spec[fl] = conv;
            final_spec[fl + 1] = 0;
            double d = lmod == 6 ? gva_longdouble(&va) : gva_double(&va);
            len = snprintf(tmp, sizeof tmp, final_spec, d);
            break;
        }
        case 'n': {
            gaddr_t dst = gva_u32(&va);
            if (dst) {
                if (lmod == 1) gwrite8(dst, (uint8_t)o.len);
                else if (lmod == 2) gwrite16(dst, (uint16_t)o.len);
                else if (lmod == 4) { gwrite32(dst, (uint32_t)o.len); gwrite32(dst + 4, 0); }
                else gwrite32(dst, (uint32_t)o.len);
            }
            continue;
        }
        default:
            out_append(&o, start, (size_t)(p - start));
            continue;
        }
        if (len > 0) {
            if ((size_t)len >= sizeof tmp) {
                // Very wide fields: fall back to dynamic formatting.
                out_append(&o, tmp, sizeof tmp - 1);
                len -= (int)sizeof tmp - 1;
                while (len-- > 0) out_append(&o, " ", 1);
            } else
                out_append(&o, tmp, (size_t)len);
        }
    }
    if (cap) out[o.len < cap - 1 ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int gfmt_vasprintf(char **out, const char *fmt, struct gva va) {
    char small[1024];
    int n = gfmt_vsnprintf(small, sizeof small, fmt, va);
    if (n < 0) return -1;
    *out = malloc((size_t)n + 1);
    if ((size_t)n < sizeof small) memcpy(*out, small, (size_t)n + 1);
    else gfmt_vsnprintf(*out, (size_t)n + 1, fmt, va);
    return n;
}

// --- scanf ---------------------------------------------------------------------
// Rewrite the format for host sizes and pass host pointers. Guest 'l' integers
// are 32-bit, so they become plain ints; results for 'L' doubles are widened.

#define MAX_SCAN_ARGS 32

int gfmt_vsscanf(const char *str, const char *fmt, struct gva va) {
    char hfmt[1024];
    size_t hl = 0;
    void *ptrs[MAX_SCAN_ARGS] = {0};
    // Post-processing for conversions that need translation.
    struct { int kind; gaddr_t dst; long double ld; } fix[MAX_SCAN_ARGS];
    int nptrs = 0;
    const char *p = fmt;
    while (*p && hl < sizeof hfmt - 8) {
        if (*p != '%') {
            hfmt[hl++] = *p++;
            continue;
        }
        hfmt[hl++] = *p++;
        if (*p == '%') {
            hfmt[hl++] = *p++;
            continue;
        }
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            hfmt[hl++] = *p++;
        }
        while (*p >= '0' && *p <= '9') hfmt[hl++] = *p++;
        int lmod = 0;
        if (*p == 'h') { lmod = 2; p++; if (*p == 'h') { lmod = 1; p++; } }
        else if (*p == 'l') { lmod = 3; p++; if (*p == 'l') { lmod = 4; p++; } }
        else if (*p == 'q' || *p == 'j') { lmod = 4; p++; }
        else if (*p == 'z' || *p == 't') { lmod = 5; p++; }
        else if (*p == 'L') { lmod = 6; p++; }
        char conv = *p;
        if (!conv) break;
        if (conv == '[') {
            // copy the scanset verbatim
            hfmt[hl++] = *p++;
            if (*p == '^') hfmt[hl++] = *p++;
            if (*p == ']') hfmt[hl++] = *p++;
            while (*p && *p != ']' && hl < sizeof hfmt - 4) hfmt[hl++] = *p++;
            if (*p) hfmt[hl++] = *p++;
        } else {
            p++;
        }
        bool is_float = strchr("eEfgGaA", conv) != NULL;
        bool is_int = strchr("dioux Xn", conv) != NULL || conv == 'X';
        if (is_int) {
            if (lmod == 1) { hfmt[hl++] = 'h'; hfmt[hl++] = 'h'; }
            else if (lmod == 2) hfmt[hl++] = 'h';
            else if (lmod == 4) { hfmt[hl++] = 'l'; hfmt[hl++] = 'l'; }
            // lmod 3/5 (32-bit long/size_t in the guest) -> plain int
            hfmt[hl++] = conv;
        } else if (is_float) {
            if (lmod == 3) hfmt[hl++] = 'l';
            else if (lmod == 6) hfmt[hl++] = 'L';
            hfmt[hl++] = conv;
        } else if (conv == 'p') {
            hfmt[hl++] = 'x'; // guest pointers are 32-bit
        } else if (conv != '[') {
            if (lmod == 3) hfmt[hl++] = 'l';
            hfmt[hl++] = conv;
        }
        if (suppress) continue;
        if (nptrs == MAX_SCAN_ARGS) break;
        gaddr_t dst = gva_u32(&va);
        if (is_float && lmod == 6) {
            fix[nptrs].kind = 1;
            fix[nptrs].dst = dst;
            ptrs[nptrs] = &fix[nptrs].ld;
        } else {
            fix[nptrs].kind = 0;
            ptrs[nptrs] = g2h(dst);
        }
        nptrs++;
    }
    hfmt[hl] = 0;
#define P(i) ptrs[i]
    int n = sscanf(str, hfmt, P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P(10), P(11), P(12),
                   P(13), P(14), P(15), P(16), P(17), P(18), P(19), P(20), P(21), P(22), P(23), P(24), P(25),
                   P(26), P(27), P(28), P(29), P(30), P(31));
#undef P
    for (int i = 0; i < nptrs; i++)
        if (fix[i].kind == 1) double_to_x87((double)fix[i].ld, g2h(fix[i].dst));
    return n;
}
