/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-256 (FIPS 180-4) and CRC-32 in plain C, so the session code stays
 * host-testable without mbedTLS. A row file hashes at card speed either way; the hardware
 * accelerator would buy nothing a logger notices.
 */
#include <string.h>

#include "espos_sdlog.h"

/* clang-format off */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
/* clang-format on */

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t p[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void espos_sdlog_sha256_init(espos_sdlog_sha256_t *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->used = 0;
}

void espos_sdlog_sha256_update(espos_sdlog_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->used;
        if (take > n) {
            take = n;
        }
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used == 64) {
            block(c->h, c->buf);
            c->used = 0;
        }
    }
}

void espos_sdlog_sha256_hex(espos_sdlog_sha256_t *c, char hex[ESPOS_SDLOG_SHA256_HEX_MAX])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    espos_sdlog_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56) {
        espos_sdlog_sha256_update(c, &pad, 1);
    }
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) {
        lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    espos_sdlog_sha256_update(c, lenbuf, 8);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(c->h[i] >> (24 - 8 * j));
            hex[8 * i + 2 * j] = digits[byte >> 4];
            hex[8 * i + 2 * j + 1] = digits[byte & 15];
        }
    }
    hex[64] = '\0';
}

uint32_t espos_sdlog_crc32(uint32_t crc, const void *data, size_t n)
{
    /* Bitwise rather than a 1 KB table: a row a second is a few dozen bytes. */
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
