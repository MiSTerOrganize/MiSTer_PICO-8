/* native_sha1.h -- SHA-1, header-only, no dependencies.
 *
 * Used by the recorder to identify content by its BYTES rather than its path
 * (see the identity design: a renamed v2 must not pass a guard and then desync
 * silently, and the same game under a different layout must not be refused).
 *
 * Header-only and static on purpose. The same bytes have to be hashed on two
 * cores whose build systems are unrelated -- one compiles src/ directly, the
 * other copies src/native_* into a patched upstream tree -- and a divergence
 * between the two implementations would be invisible: every take would simply
 * refuse to play on the other core, with a message blaming the user's content.
 * One file, included by both, removes that failure mode. It also means no
 * Makefile or link-step change on either side.
 *
 * C89-compatible: no VLAs, no declarations after statements, no stdbool.
 *
 * SHA-1 is used here as a content fingerprint, NOT as a security primitive. A
 * take is already trusted to the extent that it carries save data (see O1); an
 * attacker who can craft a collision can also just ship the save payload
 * directly. Do not repurpose this for anything where collision resistance is
 * load-bearing.
 */

#ifndef NATIVE_SHA1_H
#define NATIVE_SHA1_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define NSHA1_DIGEST_LEN 20

typedef struct {
    uint32_t h[5];
    uint64_t len;          /* total message length in BYTES */
    uint8_t  buf[64];
    size_t   buf_used;
} nsha1_ctx;

static void nsha1_init(nsha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->buf_used = 0;
}

static void nsha1_block(nsha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    uint32_t a, b, d, e, f, k, t;
    int i;

    /* Big-endian load, done byte-wise so this is endian-independent. */
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16)
             | ((uint32_t)p[i*4+2] << 8) |  (uint32_t)p[i*4+3];
    for (i = 16; i < 80; ++i) {
        t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }

    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    /* names: a,b,d,e,f stand in for a,b,c,d,e -- 'c' is the context pointer */
    for (i = 0; i < 80; ++i) {
        uint32_t bb = b, cc = d, dd = e;
        if (i < 20)      { k = 0x5A827999u; t = (bb & cc) | ((~bb) & dd); }
        else if (i < 40) { k = 0x6ED9EBA1u; t = bb ^ cc ^ dd; }
        else if (i < 60) { k = 0x8F1BBCDCu; t = (bb & cc) | (bb & dd) | (cc & dd); }
        else             { k = 0xCA62C1D6u; t = bb ^ cc ^ dd; }
        t = ((a << 5) | (a >> 27)) + t + f + k + w[i];
        f = e;
        e = d;
        d = (b << 30) | (b >> 2);
        b = a;
        a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void nsha1_update(nsha1_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += (uint64_t)n;
    if (c->buf_used) {
        size_t take = 64 - c->buf_used;
        if (take > n) take = n;
        memcpy(c->buf + c->buf_used, p, take);
        c->buf_used += take;
        p += take;
        n -= take;
        if (c->buf_used == 64) { nsha1_block(c, c->buf); c->buf_used = 0; }
    }
    while (n >= 64) { nsha1_block(c, p); p += 64; n -= 64; }
    if (n) { memcpy(c->buf, p, n); c->buf_used = n; }
}

static void nsha1_final(nsha1_ctx *c, uint8_t out[NSHA1_DIGEST_LEN])
{
    uint64_t bits = c->len * 8u;
    uint8_t tail[72];
    size_t pad;
    int i;

    /* 0x80, then zeros until length%64 == 56, then the 64-bit big-endian
     * bit-count. buf_used is < 64, so pad is at most 64 and tail is ample. */
    pad = (c->buf_used < 56) ? (56 - c->buf_used) : (120 - c->buf_used);
    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    for (i = 0; i < 8; ++i)
        tail[pad + i] = (uint8_t)(bits >> (56 - 8*i));
    nsha1_update(c, tail, pad + 8);
    /* nsha1_update just added to c->len; harmless, len is not read again. */

    for (i = 0; i < 5; ++i) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

/* Hash a whole file. Returns 0 on success, non-zero if it could not be read.
 * A failure must NEVER be treated as "hash of nothing" -- a take whose content
 * cannot be hashed has to refuse, not compare equal to another unreadable file. */
static int nsha1_file(const char *path, uint8_t out[NSHA1_DIGEST_LEN])
{
    nsha1_ctx c;
    FILE *f;
    uint8_t buf[8192];
    size_t n;

    f = fopen(path, "rb");
    if (!f) return -1;
    nsha1_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        nsha1_update(&c, buf, n);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    nsha1_final(&c, out);
    return 0;
}

/* Hash a bounded window of an already-open file: n bytes from offset off.
 * This is what OpenBOR's bounded PAK hash needs -- full-hashing a 400 MB-1.4 GB
 * pak was measured at 20-70 s per arm and rejected. Returns 0 on success.
 * A short read is an ERROR, not a partial hash: silently hashing fewer bytes
 * would make a truncated pak compare equal to nothing in particular. */
static int nsha1_update_range(nsha1_ctx *c, FILE *f, long off, size_t n)
{
    uint8_t buf[8192];
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    while (n) {
        size_t want = (n < sizeof(buf)) ? n : sizeof(buf);
        size_t got  = fread(buf, 1, want, f);
        if (got != want) return -1;
        nsha1_update(c, buf, got);
        n -= got;
    }
    return 0;
}

/* 40-char lowercase hex, NUL-terminated. out must be >= 41 bytes.
 * For log lines and on-screen messages only -- comparisons use the raw 20 bytes. */
static void nsha1_hex(const uint8_t d[NSHA1_DIGEST_LEN], char *out)
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < NSHA1_DIGEST_LEN; ++i) {
        out[i*2]   = hexd[(d[i] >> 4) & 0xF];
        out[i*2+1] = hexd[d[i] & 0xF];
    }
    out[NSHA1_DIGEST_LEN*2] = 0;
}

/* Self-test against the two standard vectors. Returns 0 if both pass.
 * Called once at recorder init on both cores: a silently-wrong SHA-1 would make
 * every take refuse on the other core and blame the user's content for it. */
static int nsha1_selftest(void)
{
    nsha1_ctx c;
    uint8_t d[NSHA1_DIGEST_LEN];
    char hex[41];

    nsha1_init(&c);
    nsha1_update(&c, "abc", 3);
    nsha1_final(&c, d);
    nsha1_hex(d, hex);
    if (strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") != 0) return -1;

    nsha1_init(&c);
    nsha1_update(&c, "", 0);
    nsha1_final(&c, d);
    nsha1_hex(d, hex);
    if (strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") != 0) return -2;

    return 0;
}

#endif /* NATIVE_SHA1_H */
