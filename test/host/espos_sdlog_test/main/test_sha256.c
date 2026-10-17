/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "espos_sdlog.h"
#include "unity.h"

static void hash(const char *data, size_t n, char hex[ESPOS_SDLOG_SHA256_HEX_MAX])
{
    espos_sdlog_sha256_t c;
    espos_sdlog_sha256_init(&c);
    espos_sdlog_sha256_update(&c, data, n);
    espos_sdlog_sha256_hex(&c, hex);
}

TEST_CASE("sha256 matches the FIPS 180-4 vectors", "[sha256]")
{
    char hex[ESPOS_SDLOG_SHA256_HEX_MAX];
    hash("", 0, hex);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
    hash("abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    hash(two, strlen(two), hex);
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", hex);
}

TEST_CASE("sha256 is the same fed in pieces as in one go", "[sha256]")
{
    char block[1000];
    memset(block, 'a', sizeof(block));
    espos_sdlog_sha256_t c;
    espos_sdlog_sha256_init(&c);
    for (int i = 0; i < 1000; i++) {
        espos_sdlog_sha256_update(&c, block, sizeof(block));
    }
    char hex[ESPOS_SDLOG_SHA256_HEX_MAX];
    espos_sdlog_sha256_hex(&c, hex);
    /* A million 'a's, the third FIPS vector, across many block boundaries. */
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", hex);

    const char *msg = "a row,with,fields\nand another,row\n";
    char whole[ESPOS_SDLOG_SHA256_HEX_MAX], parts[ESPOS_SDLOG_SHA256_HEX_MAX];
    hash(msg, strlen(msg), whole);
    espos_sdlog_sha256_init(&c);
    for (const char *p = msg; *p; p++) {
        espos_sdlog_sha256_update(&c, p, 1);
    }
    espos_sdlog_sha256_hex(&c, parts);
    TEST_ASSERT_EQUAL_STRING(whole, parts);
}

TEST_CASE("crc32 matches zlib's check value, in one go or in pieces", "[crc32]")
{
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, espos_sdlog_crc32(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, espos_sdlog_crc32(espos_sdlog_crc32(0, "1234", 4), "56789", 5));
    TEST_ASSERT_EQUAL_HEX32(0, espos_sdlog_crc32(0, "", 0));
}
