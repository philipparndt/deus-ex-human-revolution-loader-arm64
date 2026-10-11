// <wchar.h> and <wctype.h>. wchar_t/wint_t are 32-bit and mbstate_t is 128
// bytes on both sides, so most calls pass guest pointers straight through.
// Pointer results are translated back to guest addresses.
#include "common.h"
#include "libc.h"

#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define WS(i) ((const wchar_t *)g2h(ARG(i)))
#define WP(i) ((wchar_t *)g2h(ARG(i)))
#define PS(i) ((mbstate_t *)g2h(ARG(i))) // NULL -> host internal state
#define RET_WPTR(expr)                                                                             \
    do {                                                                                           \
        const void *p_ = (expr);                                                                   \
        RET(p_ ? h2g(p_) : 0);                                                                     \
    } while (0)
// size_t results where (size_t)-1 / -2 signal errors.
#define RET_MBSZ(expr)                                                                             \
    do {                                                                                           \
        errno = 0;                                                                                 \
        size_t r_ = (expr);                                                                        \
        if (r_ == (size_t)-1) set_gerrno(errno ? errno : EILSEQ);                                 \
        RET((uint32_t)r_);                                                                         \
    } while (0)

// --- wide strings ---------------------------------------------------------------------
THUNK(wcslen) { RET(wcslen(WS(0))); }
THUNK(wcsnlen) { RET(wcsnlen(WS(0), ARG(1))); }
THUNK(wcscmp) { RET(wcscmp(WS(0), WS(1))); }
THUNK(wcsncmp) { RET(wcsncmp(WS(0), WS(1), ARG(2))); }
THUNK(wcscasecmp) { RET(wcscasecmp(WS(0), WS(1))); }
THUNK(wcsncasecmp) { RET(wcsncasecmp(WS(0), WS(1), ARG(2))); }
THUNK(wcscoll) { RET(wcscoll(WS(0), WS(1))); }
THUNK(wcsxfrm) { RET(wcsxfrm(WP(0), WS(1), ARG(2))); }
THUNK(wcscpy) { wcscpy(WP(0), WS(1)); RET(ARG(0)); }
THUNK(wcsncpy) { wcsncpy(WP(0), WS(1), ARG(2)); RET(ARG(0)); }
THUNK(wcscat) { wcscat(WP(0), WS(1)); RET(ARG(0)); }
THUNK(wcsncat) { wcsncat(WP(0), WS(1), ARG(2)); RET(ARG(0)); }
THUNK(wcslcpy) { RET(wcslcpy(WP(0), WS(1), ARG(2))); }
THUNK(wcslcat) { RET(wcslcat(WP(0), WS(1), ARG(2))); }
THUNK(wcschr) { RET_WPTR(wcschr(WS(0), (wchar_t)ARG(1))); }
THUNK(wcsrchr) { RET_WPTR(wcsrchr(WS(0), (wchar_t)ARG(1))); }
THUNK(wcsstr) { RET_WPTR(wcsstr(WS(0), WS(1))); }
THUNK(wcspbrk) { RET_WPTR(wcspbrk(WS(0), WS(1))); }
THUNK(wcsspn) { RET(wcsspn(WS(0), WS(1))); }
THUNK(wcscspn) { RET(wcscspn(WS(0), WS(1))); }
THUNK(wcsdup) {
    uint32_t n = (uint32_t)wcslen(WS(0)) + 1;
    RET(gheap_memdup(WS(0), n * 4));
}
THUNK(wcswidth) { RET(wcswidth(WS(0), ARG(1))); }
THUNK(wcwidth) { RET(wcwidth((wchar_t)ARG(0))); }
THUNK(wcstok) {
    // wcstok(s, delim, wchar_t **last): the saved pointer is a guest pointer.
    wchar_t *last = g2h(gread32(ARG(2)));
    wchar_t *t = wcstok(WP(0), WS(1), &last);
    gwrite32(ARG(2), last ? h2g(last) : 0);
    RET(t ? h2g(t) : 0);
}

THUNK(wmemchr) { RET_WPTR(wmemchr(WS(0), (wchar_t)ARG(1), ARG(2))); }
THUNK(wmemcmp) { RET(wmemcmp(WS(0), WS(1), ARG(2))); }
THUNK(wmemcpy) { memmove(WP(0), WS(1), (size_t)ARG(2) * 4); RET(ARG(0)); }
THUNK(wmemmove) { memmove(WP(0), WS(1), (size_t)ARG(2) * 4); RET(ARG(0)); }
THUNK(wmemset) { wmemset(WP(0), (wchar_t)ARG(1), ARG(2)); RET(ARG(0)); }

// --- numeric conversions (end pointers map back to the guest) -------------------------
static void put_wend(gaddr_t endp, gaddr_t base, const wchar_t *hbase, const wchar_t *end) {
    if (endp) gwrite32(endp, base + 4 * (uint32_t)(end - hbase));
}
THUNK(wcstol) {
    wchar_t *end;
    errno = 0;
    long v = wcstol(WS(0), &end, ARGI(2));
    if (v > INT32_MAX) { v = INT32_MAX; errno = ERANGE; }
    if (v < INT32_MIN) { v = INT32_MIN; errno = ERANGE; }
    if (errno) set_gerrno(errno);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RET(v);
}
THUNK(wcstoul) {
    wchar_t *end;
    errno = 0;
    unsigned long v = wcstoul(WS(0), &end, ARGI(2));
    const wchar_t *s = WS(0);
    while (iswspace((wint_t)*s)) s++;
    if (*s == L'-') v = (uint32_t)v;
    else if (v > UINT32_MAX) { v = UINT32_MAX; errno = ERANGE; }
    if (errno) set_gerrno(errno);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RET(v);
}
THUNK(wcstoll) {
    wchar_t *end;
    errno = 0;
    long long v = wcstoll(WS(0), &end, ARGI(2));
    if (errno) set_gerrno(errno);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RET64(v);
}
THUNK(wcstoull) {
    wchar_t *end;
    errno = 0;
    unsigned long long v = wcstoull(WS(0), &end, ARGI(2));
    if (errno) set_gerrno(errno);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RET64(v);
}
THUNK(wcstod) {
    wchar_t *end;
    double v = wcstod(WS(0), &end);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RETD(v);
}
THUNK(wcstof) {
    wchar_t *end;
    float v = wcstof(WS(0), &end);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RETD(v);
}
THUNK(wcstold) { // long double results are returned with double precision
    wchar_t *end;
    double v = wcstod(WS(0), &end);
    put_wend(ARG(1), ARG(0), WS(0), end);
    RETD(v);
}

// --- multibyte <-> wide ---------------------------------------------------------------------
THUNK(btowc) { RET(btowc(ARGI(0))); }
THUNK(wctob) { RET(wctob((wint_t)ARG(0))); }
THUNK(mbsinit) { RET(mbsinit(PS(0))); }
THUNK(mbrtowc) { RET_MBSZ(mbrtowc(WP(0), ARG(1) ? ARGS(1) : NULL, ARG(2), PS(3))); }
THUNK(mbrlen) { RET_MBSZ(mbrlen(ARG(0) ? ARGS(0) : NULL, ARG(1), PS(2))); }
THUNK(wcrtomb) { RET_MBSZ(wcrtomb(ARG(0) ? ARGP(0) : NULL, (wchar_t)ARG(1), PS(2))); }
THUNK(mbtowc) { RET(mbtowc(WP(0), ARG(1) ? ARGS(1) : NULL, ARG(2))); }
THUNK(wctomb) { RET(wctomb(ARG(0) ? ARGP(0) : NULL, (wchar_t)ARG(1))); }
THUNK(mblen) { RET(mblen(ARG(0) ? ARGS(0) : NULL, ARG(1))); }
THUNK(mbstowcs) { RET_MBSZ(mbstowcs(WP(0), ARGS(1), ARG(2))); }
THUNK(wcstombs) { RET_MBSZ(wcstombs(ARG(0) ? ARGP(0) : NULL, WS(1), ARG(2))); }

// The *srtombs/*srtowcs family update a guest pointer-to-pointer (*src), but only when dst != NULL.
THUNK(mbsrtowcs) {
    const char *src = g2h(gread32(ARG(1)));
    errno = 0;
    size_t rc = mbsrtowcs(WP(0), &src, ARG(2), PS(3));
    if (rc == (size_t)-1) set_gerrno(errno ? errno : EILSEQ);
    if (ARG(0)) gwrite32(ARG(1), src ? h2g(src) : 0);
    RET((uint32_t)rc);
}
THUNK(mbsnrtowcs) {
    const char *src = g2h(gread32(ARG(1)));
    errno = 0;
    size_t rc = mbsnrtowcs(WP(0), &src, ARG(2), ARG(3), PS(4));
    if (rc == (size_t)-1) set_gerrno(errno ? errno : EILSEQ);
    if (ARG(0)) gwrite32(ARG(1), src ? h2g(src) : 0);
    RET((uint32_t)rc);
}
THUNK(wcsrtombs) {
    const wchar_t *src = g2h(gread32(ARG(1)));
    errno = 0;
    size_t rc = wcsrtombs(ARG(0) ? ARGP(0) : NULL, &src, ARG(2), PS(3));
    if (rc == (size_t)-1) set_gerrno(errno ? errno : EILSEQ);
    if (ARG(0)) gwrite32(ARG(1), src ? h2g(src) : 0);
    RET((uint32_t)rc);
}
THUNK(wcsnrtombs) {
    const wchar_t *src = g2h(gread32(ARG(1)));
    errno = 0;
    size_t rc = wcsnrtombs(ARG(0) ? ARGP(0) : NULL, &src, ARG(2), ARG(3), PS(4));
    if (rc == (size_t)-1) set_gerrno(errno ? errno : EILSEQ);
    if (ARG(0)) gwrite32(ARG(1), src ? h2g(src) : 0);
    RET((uint32_t)rc);
}

// --- wctype ----------------------------------------------------------------------------------
// wctype_t and wctrans_t are 32-bit values on both sides (not pointers).
THUNK(wctype) { RET((uint32_t)wctype(ARGS(0))); }
THUNK(iswctype) { RET(iswctype((wint_t)ARG(0), (wctype_t)ARG(1))); }
THUNK(wctrans) { RET((uint32_t)(uintptr_t)wctrans(ARGS(0))); }
THUNK(towctrans) { RET(towctrans((wint_t)ARG(0), (wctrans_t)(uintptr_t)ARG(1))); }
THUNK(towlower) { RET(towlower((wint_t)ARG(0))); }
THUNK(towupper) { RET(towupper((wint_t)ARG(0))); }
#define ISW(fn) THUNK(fn) { RET(fn((wint_t)ARG(0))); }
ISW(iswalpha) ISW(iswdigit) ISW(iswspace) ISW(iswupper) ISW(iswlower) ISW(iswalnum)
ISW(iswpunct) ISW(iswprint) ISW(iswxdigit) ISW(iswcntrl) ISW(iswgraph) ISW(iswblank)

// --- wide scanf ---------------------------------------------------------------------------------
// The input and format are converted to UTF-8 and scanned by the narrow
// scanner (%s stores multibyte text, %ls converts back to wide characters, as
// required for wide scanf). Exact for ASCII input.
static char *wide_to_utf8(const wchar_t *w) {
    size_t n = wcslen(w);
    char *s = malloc(n * 4 + 1), *p = s;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c < 0x80) *p++ = (char)c;
        else if (c < 0x800) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            *p++ = (char)(0xE0 | c >> 12);
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xF0 | c >> 18);
            *p++ = (char)(0x80 | ((c >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = 0;
    return s;
}
static int do_vswscanf(const wchar_t *str, const wchar_t *fmt, struct gva va) {
    char *s = wide_to_utf8(str), *f = wide_to_utf8(fmt);
    int rc = gfmt_vsscanf(s, f, va);
    free(s);
    free(f);
    return rc;
}
THUNK(vswscanf) { RET(do_vswscanf(WS(0), WS(1), gva_at(ARG(2)))); }
THUNK(swscanf) { RET(do_vswscanf(WS(0), WS(1), GVA_FROM_ARG(2))); }
