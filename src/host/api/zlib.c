// libz and libbz2: streams are converted between the i386 guest layout and a
// host stream kept per guest stream (its handle lives in the guest 'state' field).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "common.h"

#include <bzlib.h>
#include <zlib.h>
#include "ret_return.h"

__asm__(".linker_option \"-lz\"");
__asm__(".linker_option \"-lbz2\"");

// --- z_stream (i386, 56 bytes) ---------------------------------------------------------
enum {
    GZ_NEXT_IN = 0, GZ_AVAIL_IN = 4, GZ_TOTAL_IN = 8, GZ_NEXT_OUT = 12, GZ_AVAIL_OUT = 16, GZ_TOTAL_OUT = 20,
    GZ_MSG = 24, GZ_STATE = 28, GZ_ZALLOC = 32, GZ_ZFREE = 36, GZ_OPAQUE = 40, GZ_DATA_TYPE = 44,
    GZ_ADLER = 48, GZ_RESERVED = 52, GZ_SIZE = 56
};

// zlib messages are static host strings; give each a stable guest copy.
static gaddr_t guest_msg(const char *m) {
    static struct { const char *h; gaddr_t g; } cache[32];
    static int n;
    if (!m) return 0;
    for (int i = 0; i < n; i++)
        if (cache[i].h == m) return cache[i].g;
    gaddr_t g = gheap_strdup(m);
    if (n < 32) cache[n].h = m, cache[n++].g = g;
    return g;
}

static z_stream *zs_get(gaddr_t g) {
    z_stream *s = hobj_unwrap(gread32(g + GZ_STATE));
    if (!s) LOGE("zlib: guest stream %#x has no host state", g);
    return s;
}

static void zs_in(gaddr_t g, z_stream *s) {
    s->next_in = g2h(gread32(g + GZ_NEXT_IN));
    s->avail_in = gread32(g + GZ_AVAIL_IN);
    s->next_out = g2h(gread32(g + GZ_NEXT_OUT));
    s->avail_out = gread32(g + GZ_AVAIL_OUT);
    s->total_in = gread32(g + GZ_TOTAL_IN);
    s->total_out = gread32(g + GZ_TOTAL_OUT);
}

static void zs_out(gaddr_t g, z_stream *s) {
    gwrite32(g + GZ_NEXT_IN, h2g(s->next_in));
    gwrite32(g + GZ_AVAIL_IN, s->avail_in);
    gwrite32(g + GZ_TOTAL_IN, (uint32_t)s->total_in);
    gwrite32(g + GZ_NEXT_OUT, h2g(s->next_out));
    gwrite32(g + GZ_AVAIL_OUT, s->avail_out);
    gwrite32(g + GZ_TOTAL_OUT, (uint32_t)s->total_out);
    gwrite32(g + GZ_MSG, guest_msg(s->msg));
    gwrite32(g + GZ_DATA_TYPE, (uint32_t)s->data_type);
    gwrite32(g + GZ_ADLER, (uint32_t)s->adler);
}

static int zs_check(gaddr_t version, int size) {
    if (!version || gread8(version) != '1' || size != GZ_SIZE) {
        LOGE("zlib: guest stream_size %d / version mismatch", size);
        return 0;
    }
    return 1;
}

static z_stream *zs_new(gaddr_t g) {
    if (gread32(g + GZ_ZALLOC)) LOGD("zlib: guest zalloc ignored (host allocates)");
    z_stream *s = calloc(1, sizeof *s);
    zs_in(g, s);
    return s;
}

static void zs_init_done(gaddr_t g, z_stream *s, int rc) {
    if (rc == Z_OK) {
        gwrite32(g + GZ_STATE, hobj_wrap(s));
        zs_out(g, s);
    } else {
        gwrite32(g + GZ_MSG, guest_msg(s->msg));
        free(s);
    }
}

THUNK(inflateInit_) {
    if (!zs_check(ARG(1), ARGI(2))) RET(Z_VERSION_ERROR);
    z_stream *s = zs_new(ARG(0));
    int rc = inflateInit(s);
    zs_init_done(ARG(0), s, rc);
    RET(rc);
}
THUNK(inflateInit2_) {
    if (!zs_check(ARG(2), ARGI(3))) RET(Z_VERSION_ERROR);
    z_stream *s = zs_new(ARG(0));
    int rc = inflateInit2(s, ARGI(1));
    zs_init_done(ARG(0), s, rc);
    RET(rc);
}
THUNK(deflateInit_) {
    if (!zs_check(ARG(2), ARGI(3))) RET(Z_VERSION_ERROR);
    z_stream *s = zs_new(ARG(0));
    int rc = deflateInit(s, ARGI(1));
    zs_init_done(ARG(0), s, rc);
    RET(rc);
}
THUNK(deflateInit2_) {
    // (strm, level, method, windowBits, memLevel, strategy, version, stream_size)
    if (!zs_check(ARG(6), ARGI(7))) RET(Z_VERSION_ERROR);
    z_stream *s = zs_new(ARG(0));
    int rc = deflateInit2(s, ARGI(1), ARGI(2), ARGI(3), ARGI(4), ARGI(5));
    zs_init_done(ARG(0), s, rc);
    RET(rc);
}

#define ZS_OP(name, call)                                                                          \
    THUNK(name) {                                                                                  \
        gaddr_t g = ARG(0);                                                                        \
        z_stream *s = zs_get(g);                                                                   \
        if (!s) RET(Z_STREAM_ERROR);                                                               \
        zs_in(g, s);                                                                               \
        int rc = call;                                                                             \
        zs_out(g, s);                                                                              \
        RET(rc);                                                                                   \
    }
ZS_OP(inflate, inflate(s, ARGI(1)))
ZS_OP(deflate, deflate(s, ARGI(1)))
ZS_OP(inflateReset, inflateReset(s))
ZS_OP(deflateReset, deflateReset(s))

#define ZS_END(name, call)                                                                         \
    THUNK(name) {                                                                                  \
        gaddr_t g = ARG(0);                                                                        \
        z_stream *s = zs_get(g);                                                                   \
        if (!s) RET(Z_STREAM_ERROR);                                                               \
        zs_in(g, s);                                                                               \
        int rc = call;                                                                             \
        zs_out(g, s);                                                                              \
        hobj_forget(gread32(g + GZ_STATE));                                                        \
        gwrite32(g + GZ_STATE, 0);                                                                 \
        free(s);                                                                                   \
        RET(rc);                                                                                   \
    }
ZS_END(inflateEnd, inflateEnd(s))
ZS_END(deflateEnd, deflateEnd(s))

THUNK(deflateBound) {
    z_stream *s = zs_get(ARG(0));
    RET(deflateBound(s, ARG(1)));
}
THUNK(compressBound) { RET(compressBound(ARG(0))); }

THUNK(compress) {
    uLongf dl = gread32(ARG(1));
    int rc = compress(ARGP(0), &dl, ARGP(2), ARG(3));
    gwrite32(ARG(1), (uint32_t)dl);
    RET(rc);
}
THUNK(compress2) {
    uLongf dl = gread32(ARG(1));
    int rc = compress2(ARGP(0), &dl, ARGP(2), ARG(3), ARGI(4));
    gwrite32(ARG(1), (uint32_t)dl);
    RET(rc);
}
THUNK(uncompress) {
    uLongf dl = gread32(ARG(1));
    int rc = uncompress(ARGP(0), &dl, ARGP(2), ARG(3));
    gwrite32(ARG(1), (uint32_t)dl);
    RET(rc);
}
THUNK(crc32) { RET(crc32(ARG(0), ARGP(1), ARG(2))); }
THUNK(adler32) { RET(adler32(ARG(0), ARGP(1), ARG(2))); }
THUNK(zlibVersion) {
    static gaddr_t v;
    if (!v) v = gheap_strdup(zlibVersion());
    RET(v);
}

// --- bz_stream (i386, 48 bytes) ------------------------------------------------------------
enum {
    GB_NEXT_IN = 0, GB_AVAIL_IN = 4, GB_TOTAL_IN_LO = 8, GB_TOTAL_IN_HI = 12, GB_NEXT_OUT = 16,
    GB_AVAIL_OUT = 20, GB_TOTAL_OUT_LO = 24, GB_TOTAL_OUT_HI = 28, GB_STATE = 32
};

static void bz_in(gaddr_t g, bz_stream *s) {
    s->next_in = g2h(gread32(g + GB_NEXT_IN));
    s->avail_in = gread32(g + GB_AVAIL_IN);
    s->next_out = g2h(gread32(g + GB_NEXT_OUT));
    s->avail_out = gread32(g + GB_AVAIL_OUT);
}
static void bz_out(gaddr_t g, bz_stream *s) {
    gwrite32(g + GB_NEXT_IN, h2g(s->next_in));
    gwrite32(g + GB_AVAIL_IN, s->avail_in);
    gwrite32(g + GB_TOTAL_IN_LO, s->total_in_lo32);
    gwrite32(g + GB_TOTAL_IN_HI, s->total_in_hi32);
    gwrite32(g + GB_NEXT_OUT, h2g(s->next_out));
    gwrite32(g + GB_AVAIL_OUT, s->avail_out);
    gwrite32(g + GB_TOTAL_OUT_LO, s->total_out_lo32);
    gwrite32(g + GB_TOTAL_OUT_HI, s->total_out_hi32);
}

THUNK(BZ2_bzDecompressInit) {
    gaddr_t g = ARG(0);
    bz_stream *s = calloc(1, sizeof *s);
    bz_in(g, s);
    int rc = BZ2_bzDecompressInit(s, ARGI(1), ARGI(2));
    if (rc != BZ_OK) {
        free(s);
        RET(rc);
    }
    gwrite32(g + GB_STATE, hobj_wrap(s));
    bz_out(g, s);
    RET(rc);
}
THUNK(BZ2_bzDecompress) {
    gaddr_t g = ARG(0);
    bz_stream *s = hobj_unwrap(gread32(g + GB_STATE));
    if (!s) RET(BZ_PARAM_ERROR);
    bz_in(g, s);
    int rc = BZ2_bzDecompress(s);
    bz_out(g, s);
    RET(rc);
}
THUNK(BZ2_bzDecompressEnd) {
    gaddr_t g = ARG(0);
    bz_stream *s = hobj_unwrap(gread32(g + GB_STATE));
    if (!s) RET(BZ_PARAM_ERROR);
    int rc = BZ2_bzDecompressEnd(s);
    hobj_forget(gread32(g + GB_STATE));
    gwrite32(g + GB_STATE, 0);
    free(s);
    RET(rc);
}
THUNK(BZ2_bzBuffToBuffDecompress) {
    unsigned int dl = gread32(ARG(1));
    int rc = BZ2_bzBuffToBuffDecompress(ARGP(0), &dl, ARGP(2), ARG(3), ARGI(4), ARGI(5));
    gwrite32(ARG(1), dl);
    RET(rc);
}
