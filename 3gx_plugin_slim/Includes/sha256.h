#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32
#define X25519_KEY_SIZE     32
#define STREAM_MAC_SIZE     16

#define STREAM_DIR_C2S      0x01
#define STREAM_DIR_S2C      0x02

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

typedef struct {
    sha256_ctx_t inner;
    uint8_t      opad[64];
} hmac_sha256_ctx_t;

typedef struct {
    uint8_t  enc_key[32];
    uint8_t  mac_key[32];
    uint8_t  dir;
    uint64_t counter;
    uint64_t seq;
    uint8_t  ks_buf[32];
    size_t   ks_pos;
} sha256_stream_t;

#define ROTR(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static const uint32_t k_sha256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

/* Compiler-barrier memory scrub (prevents GCC -O2/-Os Dead Store Elimination from stripping secret wipes) */
static inline void crypto_wipe(void *buf, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) {
        *p++ = 0;
    }
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k_sha256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void sha256_init(sha256_ctx_t *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static inline void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static inline void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }

    crypto_wipe(ctx, sizeof(*ctx));
}

static inline void sha256_compute(const uint8_t *data, size_t len, uint8_t out_hash[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out_hash);
}

/* RFC 2104 HMAC-SHA256 */
static inline void hmac_sha256_init(hmac_sha256_ctx_t *hctx, const uint8_t *key, size_t key_len) {
    uint8_t k_pad[64];
    uint8_t tk[32];

    if (key_len > 64) {
        sha256_compute(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    memset(k_pad, 0x36, 64);
    memset(hctx->opad, 0x5c, 64);
    for (size_t i = 0; i < key_len; ++i) {
        k_pad[i] ^= key[i];
        hctx->opad[i] ^= key[i];
    }

    sha256_init(&hctx->inner);
    sha256_update(&hctx->inner, k_pad, 64);
    crypto_wipe(k_pad, sizeof(k_pad));
    crypto_wipe(tk, sizeof(tk));
}

static inline void hmac_sha256_update(hmac_sha256_ctx_t *hctx, const uint8_t *data, size_t len) {
    sha256_update(&hctx->inner, data, len);
}

static inline void hmac_sha256_final(hmac_sha256_ctx_t *hctx, uint8_t out_mac[32]) {
    uint8_t inner_hash[32];
    sha256_final(&hctx->inner, inner_hash);

    sha256_ctx_t outer;
    sha256_init(&outer);
    sha256_update(&outer, hctx->opad, 64);
    sha256_update(&outer, inner_hash, 32);
    sha256_final(&outer, out_mac);

    crypto_wipe(inner_hash, sizeof(inner_hash));
    crypto_wipe(hctx, sizeof(*hctx));
}

/* Constant-time 16-byte MAC comparison */
static inline int crypto_verify_16(const uint8_t a[16], const uint8_t b[16]) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0) ? 0 : -1;
}

/* Constant-time 32-byte digest comparison */
static inline int crypto_verify_32(const uint8_t a[32], const uint8_t b[32]) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0) ? 0 : -1;
}

/* Constant-time check for all-zero 32-byte array (rejects low-order X25519 points) */
static inline int crypto_is_zero_32(const uint8_t a[32]) {
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; ++i) {
        acc |= a[i];
    }
    return (acc == 0) ? 1 : 0;
}

/* ============================================================================
 * RFC 7748 X25519 (Curve25519 Elliptic Curve Diffie-Hellman)
 * Constant-time Montgomery ladder over GF(2^255 - 19)
 * ============================================================================ */

typedef int64_t gf25519[16];

static inline void car25519(gf25519 o) {
    int64_t c;
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= (c << 16);
    }
}

static inline void sel25519(gf25519 p, gf25519 q, int b) {
    int64_t t, c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; ++i) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static inline void unpack25519(gf25519 o, const uint8_t n[32]) {
    for (int i = 0; i < 16; ++i)
        o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static inline void pack25519(uint8_t o[32], const gf25519 n) {
    int i, j, b;
    gf25519 m, t;
    for (i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; ++i) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static inline void add25519(gf25519 o, const gf25519 a, const gf25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

static inline void sub25519(gf25519 o, const gf25519 a, const gf25519 b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

static inline void mul25519(gf25519 o, const gf25519 a, const gf25519 b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static inline void inv25519(gf25519 o, const gf25519 i) {
    gf25519 c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        mul25519(c, c, c);
        if (a != 2 && a != 4) mul25519(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

/* Compute X25519(scalar, point) -> out[32]. Returns 0 on success, -1 if shared secret is low-order zero. */
static inline int x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t r;
    gf25519 x, a, b, c, d, e, f;
    static const gf25519 _121665 = {0xDB41, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < 32; ++i) z[i] = scalar[i];
    z[0]  &= 248;
    z[31] = (z[31] & 127) | 64;

    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
        add25519(e, a, c);
        sub25519(a, a, c);
        add25519(c, b, d);
        sub25519(b, b, d);
        mul25519(d, e, e);
        mul25519(f, a, a);
        mul25519(a, c, a);
        mul25519(c, b, e);
        add25519(e, a, c);
        sub25519(a, a, c);
        mul25519(b, a, a);
        sub25519(c, d, f);
        mul25519(a, c, _121665);
        add25519(a, a, d);
        mul25519(c, c, a);
        mul25519(a, d, f);
        mul25519(d, b, x);
        mul25519(b, e, e);
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
    }

    inv25519(c, c);
    mul25519(a, a, c);
    pack25519(out, a);
    crypto_wipe(z, sizeof(z));
    return crypto_is_zero_32(out) ? -1 : 0;
}

/* Compute X25519 public key from 32-byte secret key using basepoint u = 9 */
static inline void x25519_basepoint(uint8_t out_pub[32], const uint8_t secret[32]) {
    static const uint8_t base9[32] = {9};
    (void)x25519_scalarmult(out_pub, secret, base9);
}

/* Encode uint64_t in Big-Endian */
static inline void store_be64(uint8_t out[8], uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(val >> (56 - i * 8));
    }
}

/*
 * Initialize directional SHA-256-CTR + HMAC-SHA256 stream state from
 * X25519 shared secret + ephemeral public keys (s_pub, c_pub) + psk (Perfect Forward Secrecy):
 *   enc_key = SHA256(shared_secret || s_pub || c_pub || psk || "ENC")
 *   mac_key = SHA256(shared_secret || s_pub || c_pub || psk || "MAC")
 */
static inline void sha256_stream_init(
    sha256_stream_t *st,
    const uint8_t shared_secret[32],
    const uint8_t s_pub[32],
    const uint8_t c_pub[32],
    const char *psk,
    uint8_t dir
) {
    size_t psk_len = strlen(psk);
    sha256_ctx_t ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, shared_secret, 32);
    sha256_update(&ctx, s_pub, 32);
    sha256_update(&ctx, c_pub, 32);
    sha256_update(&ctx, (const uint8_t *)psk, psk_len);
    sha256_update(&ctx, (const uint8_t *)"ENC", 3);
    sha256_final(&ctx, st->enc_key);

    sha256_init(&ctx);
    sha256_update(&ctx, shared_secret, 32);
    sha256_update(&ctx, s_pub, 32);
    sha256_update(&ctx, c_pub, 32);
    sha256_update(&ctx, (const uint8_t *)psk, psk_len);
    sha256_update(&ctx, (const uint8_t *)"MAC", 3);
    sha256_final(&ctx, st->mac_key);

    st->dir = dir;
    st->counter = 0;
    st->seq = 0;
    st->ks_pos = 32;
}

/* XOR `len` bytes in `buf` with the SHA-256-CTR keystream: SHA256(enc_key || dir || counter_be64) */
static inline void sha256_stream_xor(sha256_stream_t *st, uint8_t *buf, size_t len) {
    uint8_t block_in[32 + 1 + 8];
    memcpy(block_in, st->enc_key, 32);
    block_in[32] = st->dir;

    for (size_t i = 0; i < len; ++i) {
        if (st->ks_pos >= 32) {
            store_be64(&block_in[33], st->counter++);
            sha256_compute(block_in, sizeof(block_in), st->ks_buf);
            st->ks_pos = 0;
        }
        buf[i] ^= st->ks_buf[st->ks_pos++];
    }
    crypto_wipe(block_in, sizeof(block_in));
}

#endif /* SHA256_H */
