// stdio: guest FILE objects backed by host FILE streams.
//
// A guest FILE is an 88-byte block (sizeof(FILE) on Darwin i386). The host
// FILE* is stored inside the block's _bf field (unused by guest code, which
// only calls stdio functions and never the *_unlocked macros).
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>

#define GFILE_SIZE 88
#define GFILE_HOST_OFF 16
#define GFILE_MAGIC_OFF 24
#define GFILE_MAGIC 0x44584649u // 'DXFI'

static gaddr_t sF; // __sF[3]

static void gfile_bind(gaddr_t g, FILE *f) {
    memset(g2h(g), 0, GFILE_SIZE);
    memcpy(g2h(g + GFILE_HOST_OFF), &f, sizeof f);
    gwrite32(g + GFILE_MAGIC_OFF, GFILE_MAGIC);
    gwrite16(g + 14, (uint16_t)fileno(f)); // _file
}

static FILE *gfile_host(gaddr_t g) {
    if (!g) return NULL;
    if (gread32(g + GFILE_MAGIC_OFF) != GFILE_MAGIC) {
        LOGE("stdio: %#x is not a guest FILE", g);
        return NULL;
    }
    FILE *f;
    memcpy(&f, g2h(g + GFILE_HOST_OFF), sizeof f);
    return f;
}

static gaddr_t gfile_new(FILE *f) {
    if (!f) return 0;
    gaddr_t g = gheap_calloc(1, GFILE_SIZE);
    gfile_bind(g, f);
    return g;
}

static void init_sF(gaddr_t a) {
    sF = a;
    gfile_bind(a, stdin);
    gfile_bind(a + GFILE_SIZE, stdout);
    gfile_bind(a + 2 * GFILE_SIZE, stderr);
}
GDATA(__sF, 3 * GFILE_SIZE, init_sF);
static void init_stdinp(gaddr_t a) { gwrite32(a, hostsym_lookup("___sF")); }
static void init_stdoutp(gaddr_t a) { gwrite32(a, hostsym_lookup("___sF") + GFILE_SIZE); }
static void init_stderrp(gaddr_t a) { gwrite32(a, hostsym_lookup("___sF") + 2 * GFILE_SIZE); }
GDATA(__stdinp, 4, init_stdinp);
GDATA(__stdoutp, 4, init_stdoutp);
GDATA(__stderrp, 4, init_stderrp);

void stdio_flush_all(void) { fflush(NULL); }

#define FP(i) gfile_host(ARG(i))

THUNK(fopen) { RET(gfile_new(fopen(ARGS(0), ARGS(1)))); }
THUNK_AS(fopen_unix2003, "_fopen$UNIX2003") { RET(gfile_new(fopen(ARGS(0), ARGS(1)))); }
THUNK(fdopen) { RET(gfile_new(fdopen(ARGI(0), ARGS(1)))); }
THUNK(freopen) {
    FILE *f = freopen(ARG(0) ? ARGS(0) : NULL, ARGS(1), FP(2));
    if (!f) RET(0);
    gfile_bind(ARG(2), f);
    RET(ARG(2));
}
THUNK(fclose) {
    FILE *f = FP(0);
    int rc = f ? fclose(f) : EOF;
    if (rc != 0) set_gerrno(errno);
    gaddr_t g = ARG(0);
    if (g && (g < sF || g >= sF + 3 * GFILE_SIZE)) {
        gwrite32(g + GFILE_MAGIC_OFF, 0);
        gheap_free(g);
    }
    RET(rc);
}
THUNK(fread) { RET(fread(ARGP(0), ARG(1), ARG(2), FP(3))); }
THUNK(fwrite) { RET(fwrite(ARGP(0), ARG(1), ARG(2), FP(3))); }
THUNK_AS(fwrite_unix2003, "_fwrite$UNIX2003") { RET(fwrite(ARGP(0), ARG(1), ARG(2), FP(3))); }
THUNK(fseek) { RET_ERRNO(fseek(FP(0), ARGI(1), ARGI(2))); }
THUNK(fseeko) { RET_ERRNO(fseeko(FP(0), (off_t)ARG64(1), ARGI(3))); }
THUNK(ftell) { RET_ERRNO(ftell(FP(0))); }
THUNK(ftello) { RET64(ftello(FP(0))); }
THUNK(rewind) { rewind(FP(0)); }
THUNK(fflush) { RET(fflush(ARG(0) ? FP(0) : NULL)); }
THUNK(fgets) {
    char *s = fgets(ARGP(0), ARGI(1), FP(2));
    RET(s ? ARG(0) : 0);
}
THUNK(fputs) { RET(fputs(ARGS(0), FP(1))); }
THUNK(puts) { RET(puts(ARGS(0))); }
THUNK(putchar) { RET(putchar(ARGI(0))); }
THUNK(fputc) { RET(fputc(ARGI(0), FP(1))); }
THUNK(putc) { RET(putc(ARGI(0), FP(1))); }
THUNK(getc) { RET(getc(FP(0))); }
THUNK(fgetc) { RET(fgetc(FP(0))); }
THUNK(getchar) { RET(getchar()); }
THUNK(ungetc) { RET(ungetc(ARGI(0), FP(1))); }
THUNK(feof) { RET(feof(FP(0))); }
THUNK(ferror) { RET(ferror(FP(0))); }
THUNK(clearerr) { clearerr(FP(0)); }
THUNK(fileno) { RET(fileno(FP(0))); }
THUNK(setvbuf) {
    // Guest buffers would be fine to use, but the host manages its own.
    RET(setvbuf(FP(0), NULL, ARGI(2), ARG(3)));
}
THUNK(setbuf) { setvbuf(FP(0), NULL, ARG(1) ? _IOFBF : _IONBF, BUFSIZ); }
THUNK(tmpfile) { RET(gfile_new(tmpfile())); }
THUNK(tmpnam) {
    char buf[L_tmpnam + 16];
    snprintf(buf, sizeof buf, "/tmp/dxh.%d.%u", getpid(), arc4random());
    if (ARG(0)) {
        gstrlcpy(ARG(0), buf, L_tmpnam);
        RET(ARG(0));
    } else
        RET(guest_static_string(1, buf));
}
THUNK(remove) { RET_ERRNO(remove(ARGS(0))); }
THUNK(rename) { RET_ERRNO(rename(ARGS(0), ARGS(1))); }
THUNK(perror) {
    int e = (int)gread32(guest_errno_addr());
    if (ARG(0) && *ARGS(0)) fprintf(stderr, "%s: %s\n", ARGS(0), strerror(e));
    else fprintf(stderr, "%s\n", strerror(e));
}

// --- formatted output ------------------------------------------------------------

static int do_fprintf(FILE *f, const char *fmt, struct gva va) {
    char *s;
    int n = gfmt_vasprintf(&s, fmt, va);
    if (n < 0) return -1;
    fwrite(s, 1, (size_t)n, f);
    free(s);
    return n;
}

static int do_vsnprintf(gaddr_t dst, uint32_t cap, const char *fmt, struct gva va) {
    if (cap == 0) return gfmt_vsnprintf(NULL, 0, fmt, va);
    return gfmt_vsnprintf(g2h(dst), cap, fmt, va);
}

THUNK(printf) { RET(do_fprintf(stdout, ARGS(0), GVA_FROM_ARG(1))); }
THUNK(vprintf) { RET(do_fprintf(stdout, ARGS(0), gva_at(ARG(1)))); }
THUNK(fprintf) { RET(do_fprintf(FP(0), ARGS(1), GVA_FROM_ARG(2))); }
THUNK(vfprintf) { RET(do_fprintf(FP(0), ARGS(1), gva_at(ARG(2)))); }
THUNK(sprintf) { RET(do_vsnprintf(ARG(0), 0x7fffffff, ARGS(1), GVA_FROM_ARG(2))); }
THUNK(vsprintf) { RET(do_vsnprintf(ARG(0), 0x7fffffff, ARGS(1), gva_at(ARG(2)))); }
THUNK(snprintf) { RET(do_vsnprintf(ARG(0), ARG(1), ARGS(2), GVA_FROM_ARG(3))); }
THUNK(vsnprintf) { RET(do_vsnprintf(ARG(0), ARG(1), ARGS(2), gva_at(ARG(3)))); }
// __sprintf_chk(dst, flag, dstlen, fmt, ...), __snprintf_chk(dst, maxlen, flag, dstlen, fmt, ...)
THUNK(__sprintf_chk) { RET(do_vsnprintf(ARG(0), ARG(2) ? ARG(2) : 0x7fffffff, ARGS(3), GVA_FROM_ARG(4))); }
THUNK(__snprintf_chk) { RET(do_vsnprintf(ARG(0), ARG(1), ARGS(4), GVA_FROM_ARG(5))); }
THUNK(__vsprintf_chk) { RET(do_vsnprintf(ARG(0), ARG(2) ? ARG(2) : 0x7fffffff, ARGS(3), gva_at(ARG(4)))); }
THUNK(__vsnprintf_chk) { RET(do_vsnprintf(ARG(0), ARG(1), ARGS(4), gva_at(ARG(5)))); }

static int do_vasprintf(gaddr_t out, const char *fmt, struct gva va) {
    char *s;
    int n = gfmt_vasprintf(&s, fmt, va);
    if (n < 0) {
        gwrite32(out, 0);
        return -1;
    }
    gwrite32(out, gheap_memdup(s, (uint32_t)n + 1));
    free(s);
    return n;
}
THUNK(asprintf) { RET(do_vasprintf(ARG(0), ARGS(1), GVA_FROM_ARG(2))); }
THUNK(vasprintf) { RET(do_vasprintf(ARG(0), ARGS(1), gva_at(ARG(2)))); }

// --- formatted input -----------------------------------------------------------------

THUNK(sscanf) { RET(gfmt_vsscanf(ARGS(0), ARGS(1), GVA_FROM_ARG(2))); }
THUNK(vsscanf) { RET(gfmt_vsscanf(ARGS(0), ARGS(1), gva_at(ARG(2)))); }

// fscanf: read the rest of the current line(s) is not generally correct, so
// read greedily as much as the format consumes by scanning from a buffered copy.
static int do_vfscanf(FILE *f, const char *fmt, struct gva va) {
    long pos = ftell(f);
    if (pos < 0) {
        LOGE("stdio: fscanf on a non-seekable stream is not supported");
        return EOF;
    }
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    if (n == 0) return EOF;
    // Append %n to learn how many characters were consumed.
    size_t fl = strlen(fmt);
    char *fmt2 = malloc(fl + 3);
    memcpy(fmt2, fmt, fl);
    memcpy(fmt2 + fl, "%n", 3);
    // Pass a trailing guest int for %n: put it on a scratch guest slot.
    gaddr_t consumed = gheap_calloc(1, 4);
    // Build a copy of the guest va list with the extra pointer appended: count conversions.
    int nconv = 0;
    for (const char *p = fmt; *p; p++)
        if (*p == '%') {
            if (p[1] == '%') { p++; continue; }
            if (p[1] != '*') nconv++;
        }
    gaddr_t va2 = gheap_alloc(4 * (uint32_t)(nconv + 1));
    for (int i = 0; i < nconv; i++) gwrite32(va2 + 4 * i, gread32(va.p + 4 * i));
    gwrite32(va2 + 4 * nconv, consumed);
    int rc = gfmt_vsscanf(buf, fmt2, gva_at(va2));
    uint32_t used = gread32(consumed);
    fseek(f, pos + (long)used, SEEK_SET);
    free(fmt2);
    gheap_free(va2);
    gheap_free(consumed);
    return rc;
}
THUNK(fscanf) { RET(do_vfscanf(FP(0), ARGS(1), GVA_FROM_ARG(2))); }
THUNK(vfscanf) { RET(do_vfscanf(FP(0), ARGS(1), gva_at(ARG(2)))); }

// --- symbol variants and wide-character stdio ---------------------------------------------------
THUNK_AS(fdopen_unix2003, "_fdopen$UNIX2003") { RET(gfile_new(fdopen(ARGI(0), ARGS(1)))); }
THUNK_AS(fputs_unix2003, "_fputs$UNIX2003") { RET(fputs(ARGS(0), FP(1))); }
THUNK_AS(freopen_unix2003, "_freopen$UNIX2003") {
    FILE *f = freopen(ARG(0) ? ARGS(0) : NULL, ARGS(1), FP(2));
    if (!f) RET(0);
    else {
        gfile_bind(ARG(2), f);
        RET(ARG(2));
    }
}
// wint_t and wchar_t are 32-bit on both sides.
THUNK(getwc) { RET(getwc(FP(0))); }
THUNK(fgetwc) { RET(fgetwc(FP(0))); }
THUNK(getwchar) { RET(getwchar()); }
THUNK(putwc) { RET(putwc((wchar_t)ARG(0), FP(1))); }
THUNK(fputwc) { RET(fputwc((wchar_t)ARG(0), FP(1))); }
THUNK(putwchar) { RET(putwchar((wchar_t)ARG(0))); }
THUNK(ungetwc) { RET(ungetwc((wint_t)ARG(0), FP(1))); }
THUNK(fgetws) {
    wchar_t *s = fgetws(ARGP(0), ARGI(1), FP(2));
    RET(s ? ARG(0) : 0);
}
THUNK(fputws) { RET(fputws(ARGP(0), FP(1))); }
THUNK(fwide) { RET(fwide(FP(0), ARGI(1))); }
