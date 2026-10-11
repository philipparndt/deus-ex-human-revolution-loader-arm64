// <string.h>, <stdlib.h> conversions, <ctype.h>, memory allocation.
#include "common.h"

#include <ctype.h>
#include <runetype.h>
#include <stdlib.h>
#include <strings.h>
#include <xlocale.h>

// --- memory -------------------------------------------------------------------
THUNK(memcpy) { memcpy(ARGP(0), ARGP(1), ARG(2)); RET(ARG(0)); }
THUNK(memmove) { memmove(ARGP(0), ARGP(1), ARG(2)); RET(ARG(0)); }
THUNK(memset) { memset(ARGP(0), ARGI(1), ARG(2)); RET(ARG(0)); }
THUNK(__memcpy_chk) { memmove(ARGP(0), ARGP(1), ARG(2)); RET(ARG(0)); }
THUNK(__memmove_chk) { memmove(ARGP(0), ARGP(1), ARG(2)); RET(ARG(0)); }
THUNK(__memset_chk) { memset(ARGP(0), ARGI(1), ARG(2)); RET(ARG(0)); }
THUNK(memcmp) { RET(memcmp(ARGP(0), ARGP(1), ARG(2))); }
THUNK(memchr) {
    void *p = memchr(ARGP(0), ARGI(1), ARG(2));
    RET(p ? h2g(p) : 0);
}
THUNK(bzero) { memset(ARGP(0), 0, ARG(1)); }
THUNK(__bzero) { memset(ARGP(0), 0, ARG(1)); }
THUNK(memset_pattern16) {
    uint8_t *d = ARGP(0);
    const uint8_t *pat = ARGP(1);
    uint32_t n = ARG(2);
    for (uint32_t i = 0; i < n; i++) d[i] = pat[i & 15];
}

THUNK(malloc) { RET(gheap_alloc(ARG(0))); }
THUNK(calloc) { RET(gheap_calloc(ARG(0), ARG(1))); }
THUNK(realloc) { RET(gheap_realloc(ARG(0), ARG(1))); }
THUNK(free) { gheap_free(ARG(0)); }
THUNK(valloc) { RET(gheap_memalign(4096, ARG(0))); }
THUNK(malloc_size) { RET(gheap_size(ARG(0))); }
THUNK(malloc_good_size) { RET((ARG(0) + 15) & ~15u); }
THUNK(posix_memalign) {
    gaddr_t p = gheap_memalign(ARG(1), ARG(2));
    if (!p) RET(ENOMEM);
    gwrite32(ARG(0), p);
    RET(0);
}

// --- strings --------------------------------------------------------------------
#define RET_GPTR(expr)                                                                             \
    do {                                                                                           \
        const void *p_ = (expr);                                                                   \
        RET(p_ ? h2g(p_) : 0);                                                                     \
    } while (0)

THUNK(strlen) { RET(strlen(ARGS(0))); }
THUNK(strnlen) { RET(strnlen(ARGS(0), ARG(1))); }
THUNK(strcmp) { RET(strcmp(ARGS(0), ARGS(1))); }
THUNK(strncmp) { RET(strncmp(ARGS(0), ARGS(1), ARG(2))); }
THUNK(strcasecmp) { RET(strcasecmp(ARGS(0), ARGS(1))); }
THUNK(strncasecmp) { RET(strncasecmp(ARGS(0), ARGS(1), ARG(2))); }
THUNK(strcoll) { RET(strcoll(ARGS(0), ARGS(1))); }
THUNK(strxfrm) { RET(strxfrm(ARGP(0), ARGS(1), ARG(2))); }
THUNK(strcpy) { strcpy(ARGP(0), ARGS(1)); RET(ARG(0)); }
THUNK(strncpy) { strncpy(ARGP(0), ARGS(1), ARG(2)); RET(ARG(0)); }
THUNK(strcat) { strcat(ARGP(0), ARGS(1)); RET(ARG(0)); }
THUNK(strncat) { strncat(ARGP(0), ARGS(1), ARG(2)); RET(ARG(0)); }
THUNK(__strcpy_chk) { strcpy(ARGP(0), ARGS(1)); RET(ARG(0)); }
THUNK(__strcat_chk) { strcat(ARGP(0), ARGS(1)); RET(ARG(0)); }
THUNK(__strncpy_chk) { strncpy(ARGP(0), ARGS(1), ARG(2)); RET(ARG(0)); }
THUNK(__strncat_chk) { strncat(ARGP(0), ARGS(1), ARG(2)); RET(ARG(0)); }
THUNK(strlcpy) { RET(strlcpy(ARGP(0), ARGS(1), ARG(2))); }
THUNK(strlcat) { RET(strlcat(ARGP(0), ARGS(1), ARG(2))); }
THUNK(strchr) { RET_GPTR(strchr(ARGS(0), ARGI(1))); }
THUNK(strrchr) { RET_GPTR(strrchr(ARGS(0), ARGI(1))); }
THUNK(index) { RET_GPTR(strchr(ARGS(0), ARGI(1))); }
THUNK(rindex) { RET_GPTR(strrchr(ARGS(0), ARGI(1))); }
THUNK(strstr) { RET_GPTR(strstr(ARGS(0), ARGS(1))); }
THUNK(strcasestr) { RET_GPTR(strcasestr(ARGS(0), ARGS(1))); }
THUNK(strnstr) { RET_GPTR(strnstr(ARGS(0), ARGS(1), ARG(2))); }
THUNK(strpbrk) { RET_GPTR(strpbrk(ARGS(0), ARGS(1))); }
THUNK(strspn) { RET(strspn(ARGS(0), ARGS(1))); }
THUNK(strcspn) { RET(strcspn(ARGS(0), ARGS(1))); }
THUNK(strdup) { RET(gheap_strdup(ARGS(0))); }
THUNK(strndup) {
    size_t n = strnlen(ARGS(0), ARG(1));
    gaddr_t d = gheap_alloc((uint32_t)n + 1);
    memcpy(g2h(d), ARGP(0), n);
    gwrite8(d + (uint32_t)n, 0);
    RET(d);
}
THUNK(strerror) { RET(guest_static_string(2, strerror(ARGI(0)))); }
THUNK(strerror_r) { RET(strerror_r(ARGI(0), ARGP(1), ARG(2))); }

// strtok keeps per-thread state in guest terms.
static _Thread_local gaddr_t strtok_last;
static gaddr_t do_strtok(gaddr_t s, const char *delim, gaddr_t *last) {
    if (!s) s = *last;
    if (!s) return 0;
    char *p = g2h(s);
    p += strspn(p, delim);
    if (!*p) {
        *last = 0;
        return 0;
    }
    char *end = p + strcspn(p, delim);
    if (*end) {
        *end = 0;
        *last = h2g(end + 1);
    } else {
        *last = 0;
    }
    return h2g(p);
}
THUNK(strtok) { RET(do_strtok(ARG(0), ARGS(1), &strtok_last)); }
THUNK(strtok_r) {
    gaddr_t last = gread32(ARG(2));
    gaddr_t t = do_strtok(ARG(0), ARGS(1), &last);
    gwrite32(ARG(2), last);
    RET(t);
}
THUNK(strsep) {
    gaddr_t s = gread32(ARG(0));
    if (!s) RET(0);
    char *p = g2h(s);
    char *e = p + strcspn(p, ARGS(1));
    if (*e) {
        *e = 0;
        gwrite32(ARG(0), h2g(e + 1));
    } else
        gwrite32(ARG(0), 0);
    RET(s);
}

// --- conversions (end pointers map back to guest addresses) ------------------------
static void put_endptr(gaddr_t endp, const char *base_host, gaddr_t base_guest, const char *end) {
    if (endp) gwrite32(endp, base_guest + (uint32_t)(end - base_host));
}
THUNK(atoi) { RET(atoi(ARGS(0))); }
THUNK(atol) { RET((int32_t)atol(ARGS(0))); }
THUNK(atoll) { RET64(atoll(ARGS(0))); }
THUNK(atof) { RETD(atof(ARGS(0))); }
THUNK(strtol) {
    char *end;
    errno = 0;
    long v = strtol(ARGS(0), &end, ARGI(2));
    if (v > INT32_MAX) { v = INT32_MAX; errno = ERANGE; }
    if (v < INT32_MIN) { v = INT32_MIN; errno = ERANGE; }
    if (errno) set_gerrno(errno);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RET(v);
}
THUNK(strtoul) {
    char *end;
    errno = 0;
    unsigned long v = strtoul(ARGS(0), &end, ARGI(2));
    // Host unsigned long is 64-bit; mimic 32-bit wraparound semantics for negatives.
    const char *s = ARGS(0);
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') v = (uint32_t)v;
    else if (v > UINT32_MAX) { v = UINT32_MAX; errno = ERANGE; }
    if (errno) set_gerrno(errno);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RET(v);
}
THUNK(strtoll) {
    char *end;
    errno = 0;
    long long v = strtoll(ARGS(0), &end, ARGI(2));
    if (errno) set_gerrno(errno);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RET64(v);
}
THUNK(strtoull) {
    char *end;
    errno = 0;
    unsigned long long v = strtoull(ARGS(0), &end, ARGI(2));
    if (errno) set_gerrno(errno);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RET64(v);
}
THUNK(strtod) {
    char *end;
    double v = strtod(ARGS(0), &end);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RETD(v);
}
THUNK(strtof) {
    char *end;
    float v = strtof(ARGS(0), &end);
    put_endptr(ARG(1), ARGS(0), ARG(0), end);
    RETD(v);
}

// --- ctype ------------------------------------------------------------------------------
// i386 _RuneLocale: magic[8], encoding[32], sgetrune, sputrune, invalid_rune,
// runetype[256], maplower[256], mapupper[256], ... (pointers are 4 bytes).
#define RL_RUNETYPE 52
#define RL_MAPLOWER (RL_RUNETYPE + 1024)
#define RL_MAPUPPER (RL_MAPLOWER + 1024)
static void init_rune_locale(gaddr_t a) {
    memcpy(g2h(a), _DefaultRuneLocale.__magic, 8);
    memcpy(g2h(a + 8), _DefaultRuneLocale.__encoding, 32);
    gwrite32(a + 48, (uint32_t)_DefaultRuneLocale.__invalid_rune);
    for (int i = 0; i < 256; i++) {
        gwrite32(a + RL_RUNETYPE + 4 * i, _DefaultRuneLocale.__runetype[i]);
        gwrite32(a + RL_MAPLOWER + 4 * i, (uint32_t)_DefaultRuneLocale.__maplower[i]);
        gwrite32(a + RL_MAPUPPER + 4 * i, (uint32_t)_DefaultRuneLocale.__mapupper[i]);
    }
}
GDATA(_DefaultRuneLocale, 4096, init_rune_locale);
static void init_mb_cur_max(gaddr_t a) { gwrite32(a, 1); }
GDATA(__mb_cur_max, 4, init_mb_cur_max);

THUNK(__maskrune) { RET(___runetype(ARGI(0)) & ARG(1)); }
THUNK(__tolower) { RET(___tolower(ARGI(0))); }
THUNK(__toupper) { RET(___toupper(ARGI(0))); }
THUNK(tolower) { RET(tolower(ARGI(0))); }
THUNK(toupper) { RET(toupper(ARGI(0))); }
THUNK(isalpha) { RET(isalpha(ARGI(0))); }
THUNK(isdigit) { RET(isdigit(ARGI(0))); }
THUNK(isspace) { RET(isspace(ARGI(0))); }
THUNK(isalnum) { RET(isalnum(ARGI(0))); }
THUNK(isupper) { RET(isupper(ARGI(0))); }
THUNK(islower) { RET(islower(ARGI(0))); }
THUNK(isprint) { RET(isprint(ARGI(0))); }
THUNK(ispunct) { RET(ispunct(ARGI(0))); }
THUNK(isxdigit) { RET(isxdigit(ARGI(0))); }

// --- sorting / searching / random ------------------------------------------------------------
static _Thread_local gaddr_t qsort_cmp;
static int qsort_tramp(const void *a, const void *b) {
    return (int32_t)gcall2(qsort_cmp, h2g(a), h2g(b));
}
THUNK(qsort) {
    gaddr_t prev = qsort_cmp;
    qsort_cmp = ARG(3);
    qsort(ARGP(0), ARG(1), ARG(2), qsort_tramp);
    qsort_cmp = prev;
}
static _Thread_local gaddr_t bsearch_cmp;
static int bsearch_tramp(const void *a, const void *b) { return (int32_t)gcall2(bsearch_cmp, h2g(a), h2g(b)); }
THUNK(bsearch) {
    gaddr_t prev = bsearch_cmp;
    bsearch_cmp = ARG(4);
    void *p = bsearch(ARGP(0), ARGP(1), ARG(2), ARG(3), bsearch_tramp);
    bsearch_cmp = prev;
    RET(p ? h2g(p) : 0);
}
THUNK(rand) { RET(rand()); }
THUNK(srand) { srand(ARG(0)); }
THUNK(random) { RET((uint32_t)random()); }
THUNK(srandom) { srandom(ARG(0)); }
THUNK(arc4random) { RET(arc4random()); }
THUNK(abs) { RET(abs(ARGI(0))); }
THUNK(labs) { RET(abs(ARGI(0))); }

// --- symbol variants ------------------------------------------------------------------------------
THUNK_AS(strtod_unix2003, "_strtod$UNIX2003") { thunk_impl_strtod(r); }
THUNK_AS(strtof_unix2003, "_strtof$UNIX2003") { thunk_impl_strtof(r); }
// long double results are returned with double precision.
THUNK(strtold) { thunk_impl_strtod(r); }
THUNK_AS(strerror_unix2003, "_strerror$UNIX2003") { thunk_impl_strerror(r); }
