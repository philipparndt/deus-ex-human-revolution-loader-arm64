// libcrypto.0.9.8 subset: RIPEMD-160 (own implementation on the guest context
// struct), BN_bin2bn and RSA_verify (PKCS#1 v1.5 via Security.framework raw RSA).
//
// Guest structs (OpenSSL 0.9.8, i386):
//   RIPEMD160_CTX { u32 A,B,C,D,E; u32 Nl,Nh; u32 data[16]; u32 num; }   96 bytes
//   BIGNUM        { BN_ULONG *d; int top; int dmax; int neg; int flags; } 20 bytes, 32-bit limbs
//   RSA           { int pad; long version; meth; engine; BIGNUM *n,*e,*d,*p,*q,*dmp1,*dmq1,*iqmp;
//                   ex_data{sk,dummy}; int references; int flags; mont n,p,q; bignum_data;
//                   blinding; mt_blinding; }                                   88 bytes
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "cf_bridge.h"

#include <Security/Security.h>
#include "ret_return.h"

__asm__(".linker_option \"-framework\", \"Security\"");

// --- RIPEMD-160 ------------------------------------------------------------------------------

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void rmd160_compress(uint32_t h[5], const uint8_t blk[64]) {
    static const uint8_t rl[80] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 7,  4,  13, 1,
                                   10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8, 3, 10, 14, 4, 9, 15, 8, 1,
                                   2, 7, 0, 6, 13, 11, 5, 12, 1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15,
                                   14, 5, 6, 2, 4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};
    static const uint8_t rr[80] = {5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12, 6, 11, 3, 7,
                                   0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2, 15, 5, 1, 3, 7, 14, 6, 9,
                                   11, 8, 12, 2, 10, 0, 4, 13, 8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13,
                                   9, 7, 10, 14, 12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};
    static const uint8_t sl[80] = {11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8, 7, 6, 8, 13,
                                   11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12, 11, 13, 6, 7, 14, 9, 13, 15,
                                   14, 8, 13, 6, 5, 12, 7, 5, 11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6,
                                   8, 6, 5, 12, 9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};
    static const uint8_t sr[80] = {8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6, 9, 13, 15, 7,
                                   12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11, 9, 7, 15, 11, 8, 6, 6, 14,
                                   12, 13, 5, 14, 13, 13, 7, 5, 15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9,
                                   12, 5, 15, 8, 8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};
    static const uint32_t kl[5] = {0x00000000, 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xA953FD4E};
    static const uint32_t kr[5] = {0x50A28BE6, 0x5C4DD124, 0x6D703EF3, 0x7A6D76E9, 0x00000000};
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = blk[4 * i] | blk[4 * i + 1] << 8 | blk[4 * i + 2] << 16 | (uint32_t)blk[4 * i + 3] << 24;
    uint32_t al = h[0], bl = h[1], cl = h[2], dl = h[3], el = h[4];
    uint32_t ar = al, br = bl, cr = cl, dr = dl, er = el;
    for (int j = 0; j < 80; j++) {
        int r = j / 16;
        uint32_t f, t;
        switch (r) {
        case 0: f = bl ^ cl ^ dl; break;
        case 1: f = (bl & cl) | (~bl & dl); break;
        case 2: f = (bl | ~cl) ^ dl; break;
        case 3: f = (bl & dl) | (cl & ~dl); break;
        default: f = bl ^ (cl | ~dl); break;
        }
        t = ROL(al + f + x[rl[j]] + kl[r], sl[j]) + el;
        al = el, el = dl, dl = ROL(cl, 10), cl = bl, bl = t;
        switch (r) {
        case 0: f = br ^ (cr | ~dr); break;
        case 1: f = (br & dr) | (cr & ~dr); break;
        case 2: f = (br | ~cr) ^ dr; break;
        case 3: f = (br & cr) | (~br & dr); break;
        default: f = br ^ cr ^ dr; break;
        }
        t = ROL(ar + f + x[rr[j]] + kr[r], sr[j]) + er;
        ar = er, er = dr, dr = ROL(cr, 10), cr = br, br = t;
    }
    uint32_t t = h[1] + cl + dr;
    h[1] = h[2] + dl + er;
    h[2] = h[3] + el + ar;
    h[3] = h[4] + al + br;
    h[4] = h[0] + bl + cr;
    h[0] = t;
}

// Context accessors on guest memory.
#define CTX_H(c) ((uint32_t *)g2h(c))          // A..E
#define CTX_NL(c) ((c) + 20)
#define CTX_NH(c) ((c) + 24)
#define CTX_DATA(c) ((uint8_t *)g2h((c) + 28))
#define CTX_NUM(c) ((c) + 92)

static void rmd_init(gaddr_t c) {
    memset(g2h(c), 0, 96);
    uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(CTX_H(c), iv, 20);
}

static void rmd_update(gaddr_t c, const uint8_t *p, size_t n) {
    uint32_t h[5];
    memcpy(h, CTX_H(c), 20);
    uint64_t bits = (gread32(CTX_NL(c)) | (uint64_t)gread32(CTX_NH(c)) << 32) + (uint64_t)n * 8;
    gwrite32(CTX_NL(c), (uint32_t)bits);
    gwrite32(CTX_NH(c), (uint32_t)(bits >> 32));
    uint32_t num = gread32(CTX_NUM(c)) & 63;
    uint8_t *buf = CTX_DATA(c);
    while (n) {
        size_t k = 64 - num < n ? 64 - num : n;
        memcpy(buf + num, p, k);
        num += (uint32_t)k, p += k, n -= k;
        if (num == 64) {
            rmd160_compress(h, buf);
            num = 0;
        }
    }
    gwrite32(CTX_NUM(c), num);
    memcpy(CTX_H(c), h, 20);
}

static void rmd_final(gaddr_t c, uint8_t md[20]) {
    uint64_t bits = gread32(CTX_NL(c)) | (uint64_t)gread32(CTX_NH(c)) << 32;
    uint8_t pad[72] = {0x80};
    uint32_t num = gread32(CTX_NUM(c)) & 63;
    size_t padlen = num < 56 ? 56 - num : 120 - num;
    rmd_update(c, pad, padlen);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (8 * i));
    rmd_update(c, len, 8);
    uint32_t *h = CTX_H(c);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) md[4 * i + j] = (uint8_t)(h[i] >> (8 * j));
}

THUNK(RIPEMD160_Init) { rmd_init(ARG(0)); RET(1); }
THUNK(RIPEMD160_Update) { rmd_update(ARG(0), ARGP(1), ARG(2)); RET(1); }
THUNK(RIPEMD160_Final) { rmd_final(ARG(1), ARGP(0)); RET(1); } // (md, ctx)
THUNK(RIPEMD160) {
    static gaddr_t sbuf;
    if (!sbuf) sbuf = gheap_calloc(1, 20);
    gaddr_t md = ARG(2) ? ARG(2) : sbuf;
    gaddr_t ctx = gheap_alloc(96);
    rmd_init(ctx);
    rmd_update(ctx, ARGP(0), ARG(1));
    rmd_final(ctx, g2h(md));
    gheap_free(ctx);
    RET(md);
}

// --- BIGNUM / RSA ----------------------------------------------------------------------------------

THUNK(CRYPTO_set_mem_functions) {
    LOGD("crypto: CRYPTO_set_mem_functions(%#x, %#x, %#x) ignored", ARG(0), ARG(1), ARG(2));
    RET(1);
}

// BN_bin2bn(const unsigned char *s, int len, BIGNUM *ret)
THUNK(BN_bin2bn) {
    const uint8_t *s = ARGP(0);
    int len = ARGI(1);
    gaddr_t bn = ARG(2);
    if (!bn) {
        bn = gheap_calloc(1, 20);
        gwrite32(bn + 16, 1); // BN_FLG_MALLOCED
    }
    while (len > 0 && *s == 0) s++, len--;
    uint32_t words = ((uint32_t)len + 3) / 4;
    gaddr_t d = gread32(bn);
    if ((uint32_t)gread32(bn + 8) < words || !d) {
        if (d && !(gread32(bn + 16) & 2)) gheap_free(d);
        d = gheap_calloc(words ? words : 1, 4);
        gwrite32(bn, d);
        gwrite32(bn + 8, words ? words : 1);
    }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            int idx = len - 1 - (int)(i * 4) - b;
            if (idx >= 0) w |= (uint32_t)s[idx] << (8 * b);
        }
        gwrite32(d + 4 * i, w);
    }
    gwrite32(bn + 4, words); // top (leading zero bytes stripped -> top word nonzero)
    gwrite32(bn + 12, 0);
    LOGD("crypto: BN_bin2bn(%d bytes) -> %#x", ARGI(1), bn);
    RET(bn);
}

static void bn_free(gaddr_t bn) {
    if (!bn) return;
    uint32_t flags = gread32(bn + 16);
    if (gread32(bn) && !(flags & 2)) gheap_free(gread32(bn));
    if (flags & 1) gheap_free(bn);
}

THUNK(BN_free) { bn_free(ARG(0)); }
THUNK(BN_new) {
    gaddr_t bn = gheap_calloc(1, 20);
    gwrite32(bn + 16, 1);
    RET(bn);
}

#define RSA_SIZE 0x58
THUNK(RSA_new) {
    gaddr_t rsa = gheap_calloc(1, RSA_SIZE + 0x28);
    gwrite32(rsa + 56, 1); // references
    LOGD("crypto: RSA_new -> %#x", rsa);
    RET(rsa);
}
THUNK(RSA_free) {
    gaddr_t rsa = ARG(0);
    if (!rsa) return;
    for (int i = 0; i < 8; i++) bn_free(gread32(rsa + 16 + 4 * i));
    gheap_free(rsa);
}

// Big-endian bytes of a guest BIGNUM (no leading zeros). Returns malloc'ed buffer.
static uint8_t *bn_bytes(gaddr_t bn, size_t *len) {
    *len = 0;
    if (!bn) return NULL;
    uint32_t top = gread32(bn + 4);
    gaddr_t d = gread32(bn);
    uint8_t *out = malloc(top * 4 + 1);
    size_t n = 0;
    for (int i = (int)top - 1; i >= 0; i--) {
        uint32_t w = gread32(d + 4 * (uint32_t)i);
        for (int b = 3; b >= 0; b--) {
            uint8_t c = (uint8_t)(w >> (8 * b));
            if (n == 0 && c == 0) continue;
            out[n++] = c;
        }
    }
    *len = n;
    return out;
}

static void der_len(uint8_t **p, size_t len) {
    if (len < 128) *(*p)++ = (uint8_t)len;
    else if (len < 256) *(*p)++ = 0x81, *(*p)++ = (uint8_t)len;
    else *(*p)++ = 0x82, *(*p)++ = (uint8_t)(len >> 8), *(*p)++ = (uint8_t)len;
}
static void der_int(uint8_t **p, const uint8_t *v, size_t n) {
    bool pad = n && (v[0] & 0x80);
    *(*p)++ = 0x02;
    der_len(p, n + pad);
    if (pad) *(*p)++ = 0;
    memcpy(*p, v, n);
    *p += n;
}

// RSA_verify(int type, const unsigned char *m, unsigned m_len, const unsigned char *sig, unsigned siglen, RSA *rsa)
THUNK(RSA_verify) {
    int type = ARGI(0);
    const uint8_t *m = ARGP(1), *sig = ARGP(3);
    uint32_t mlen = ARG(2), siglen = ARG(4);
    gaddr_t rsa = ARG(5);
    size_t nlen, elen;
    uint8_t *n = bn_bytes(gread32(rsa + 16), &nlen), *e = bn_bytes(gread32(rsa + 20), &elen);
    int ok = 0;
    LOGI("crypto: RSA_verify(type %d, %u-byte digest, %u-byte sig, %zu-bit key)", type, mlen, siglen, nlen * 8);
    if (!n || !e || !nlen || siglen != nlen) goto out;
    {
        uint8_t *der = malloc(nlen + elen + 32), *p = der + 4, *q;
        der_int(&p, n, nlen);
        der_int(&p, e, elen);
        size_t body = (size_t)(p - (der + 4));
        // re-emit header with correct length
        uint8_t *hdr = malloc(body + 8);
        q = hdr;
        *q++ = 0x30;
        der_len(&q, body);
        memcpy(q, der + 4, body);
        q += body;
        CFDataRef keydata = CFDataCreate(NULL, hdr, q - hdr);
        free(der);
        free(hdr);
        const void *ks[] = {kSecAttrKeyType, kSecAttrKeyClass};
        const void *vs[] = {kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, ks, vs, 2, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CFErrorRef err = NULL;
        SecKeyRef key = SecKeyCreateWithData(keydata, attrs, &err);
        CFRelease(attrs);
        CFRelease(keydata);
        if (!key) {
            LOGE("crypto: RSA_verify: cannot create public key");
            if (err) CFRelease(err);
            goto out;
        }
        CFDataRef sd = CFDataCreate(NULL, sig, siglen);
        CFDataRef dec = SecKeyCreateEncryptedData(key, kSecKeyAlgorithmRSAEncryptionRaw, sd, &err);
        CFRelease(sd);
        CFRelease(key);
        if (!dec) {
            if (err) CFRelease(err);
            goto out;
        }
        const uint8_t *b = CFDataGetBytePtr(dec);
        size_t bl = (size_t)CFDataGetLength(dec);
        // EM = 00 01 FF..FF 00 T
        static const uint8_t md5[] = {0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48,
                                      0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10};
        static const uint8_t sha1[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
                                       0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
        static const uint8_t rmd[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x24,
                                      0x03, 0x02, 0x01, 0x05, 0x00, 0x04, 0x14};
        static const uint8_t sha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                         0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
        const uint8_t *prefix = NULL;
        size_t plen = 0;
        switch (type) {
        case 4: prefix = md5, plen = sizeof md5; break;       // NID_md5
        case 64: prefix = sha1, plen = sizeof sha1; break;    // NID_sha1
        case 117: prefix = rmd, plen = sizeof rmd; break;     // NID_ripemd160
        case 672: prefix = sha256, plen = sizeof sha256; break; // NID_sha256
        case 114: break;                                       // NID_md5_sha1: raw 36 bytes
        default: LOGE("crypto: RSA_verify: unsupported digest type %d", type);
        }
        size_t tlen = plen + mlen;
        if (bl == nlen && bl >= tlen + 11 && b[0] == 0 && b[1] == 1) {
            size_t i = 2;
            while (i < bl && b[i] == 0xFF) i++;
            if (i >= 10 && i < bl && b[i] == 0 && bl - i - 1 == tlen &&
                (!plen || memcmp(b + i + 1, prefix, plen) == 0) && memcmp(b + i + 1 + plen, m, mlen) == 0)
                ok = 1;
        }
        CFRelease(dec);
    }
out:
    LOGI("crypto: RSA_verify -> %d", ok);
    free(n);
    free(e);
    RET(ok);
}
