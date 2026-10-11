// zlib / libbz2 round trips under the guest ABI.
#include <bzlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

_Static_assert(sizeof(z_stream) == 56, "i386 z_stream");
_Static_assert(offsetof(z_stream, state) == 28, "z_stream.state");
_Static_assert(sizeof(bz_stream) == 48, "i386 bz_stream");

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

static const unsigned char bz[] = {
    0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x7f, 0x9d, 0x3d, 0xa2, 0x00, 0x00, 0x06, 0xdd,
    0x80, 0x00, 0x10, 0x60, 0x00, 0x10, 0x00, 0x00, 0x40, 0x17, 0x66, 0xdc, 0x50, 0x20, 0x00, 0x31, 0x4c, 0x26,
    0x9a, 0x03, 0x4c, 0x40, 0xaa, 0xa6, 0x86, 0x9a, 0x06, 0x86, 0xca, 0x66, 0xba, 0xea, 0xb0, 0xf5, 0x6b, 0x50,
    0xdd, 0x44, 0x25, 0x76, 0x1d, 0x28, 0x86, 0x10, 0x85, 0x52, 0x94, 0xbf, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90,
    0x7f, 0x9d, 0x3d, 0xa2};

int main(void) {
    CHECK(crc32(0, (const Bytef *)"123456789", 9) == 0xcbf43926UL);
    CHECK(adler32(1, (const Bytef *)"Wikipedia", 9) == 0x11e60398UL);

    static char src[20000], comp[30000], out[20000];
    for (int i = 0; i < (int)sizeof src; i++) src[i] = "deus ex machina "[i % 16] + (i / 997 % 3);
    uLongf cl = sizeof comp, ol = sizeof out;
    CHECK(compress((Bytef *)comp, &cl, (const Bytef *)src, sizeof src) == Z_OK);
    CHECK(cl > 0 && cl < sizeof src);
    CHECK(uncompress((Bytef *)out, &ol, (const Bytef *)comp, cl) == Z_OK);
    CHECK(ol == sizeof src && memcmp(src, out, sizeof src) == 0);
    printf("compress: %d -> %lu bytes\n", (int)sizeof src, cl);

    // Streaming deflate with small output chunks.
    z_stream s;
    memset(&s, 0, sizeof s);
    CHECK(deflateInit(&s, 6) == Z_OK);
    s.next_in = (Bytef *)src;
    s.avail_in = sizeof src;
    size_t clen = 0;
    int rc;
    do {
        s.next_out = (Bytef *)comp + clen;
        s.avail_out = 100;
        rc = deflate(&s, Z_FINISH);
        clen += 100 - s.avail_out;
    } while (rc == Z_OK);
    CHECK(rc == Z_STREAM_END);
    CHECK(s.total_out == clen && s.total_in == sizeof src);
    CHECK(deflateEnd(&s) == Z_OK);

    // Streaming inflate, small input chunks.
    memset(&s, 0, sizeof s);
    memset(out, 0, sizeof out);
    CHECK(inflateInit(&s) == Z_OK);
    s.next_out = (Bytef *)out;
    s.avail_out = sizeof out;
    size_t pos = 0;
    do {
        s.next_in = (Bytef *)comp + pos;
        s.avail_in = clen - pos < 77 ? (uInt)(clen - pos) : 77;
        pos += s.avail_in;
        rc = inflate(&s, Z_NO_FLUSH);
        pos -= s.avail_in;
    } while (rc == Z_OK);
    CHECK(rc == Z_STREAM_END);
    CHECK(s.total_out == sizeof src && memcmp(src, out, sizeof src) == 0);
    CHECK(inflateReset(&s) == Z_OK);
    CHECK(inflateEnd(&s) == Z_OK);

    // Bad data -> error with a message.
    memset(&s, 0, sizeof s);
    CHECK(inflateInit2(&s, 15) == Z_OK);
    s.next_in = (Bytef *)"garbage!";
    s.avail_in = 8;
    s.next_out = (Bytef *)out;
    s.avail_out = 100;
    CHECK(inflate(&s, Z_NO_FLUSH) == Z_DATA_ERROR);
    CHECK(s.msg != NULL);
    if (s.msg) printf("zlib error message: %s\n", s.msg);
    inflateEnd(&s);

    // bzip2 decompression of an embedded blob.
    bz_stream b;
    memset(&b, 0, sizeof b);
    CHECK(BZ2_bzDecompressInit(&b, 0, 0) == BZ_OK);
    b.next_in = (char *)bz;
    b.avail_in = sizeof bz;
    b.next_out = out;
    b.avail_out = sizeof out;
    CHECK(BZ2_bzDecompress(&b) == BZ_STREAM_END);
    CHECK(b.total_out_lo32 == 50 && memcmp(out, "Hello bzip2 from dxhost! Hello", 30) == 0);
    CHECK(BZ2_bzDecompressEnd(&b) == BZ_OK);

    printf("cf_zlib: %s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
