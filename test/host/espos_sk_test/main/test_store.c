/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent SK state on real NVS (host flash emulation): clientId is
 * generated once and kept, token/pending survive a "reboot", empties erase.
 */
#include <string.h>
#include "unity.h"
#include "esp_partition.h"
#include "esp_private/partition_linux.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "espos_sk_priv.h"

static void fresh(void)
{
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(nvs_flash_erase_partition("nvs"));
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
}

TEST_CASE("store: clientId generated once and stable across loads", "[sk_store]")
{
    fresh();
    espos_sk_tok_store_t st;
    char id1[40], id2[40];
    TEST_ESP_OK(espos_sk_store_load(&st, id1));
    TEST_ASSERT_EQUAL(36, strlen(id1));
    TEST_ASSERT_EQUAL('4', id1[14]);                                   /* UUID v4 */
    TEST_ASSERT_TRUE(id1[19] == '8' || id1[19] == '9' || id1[19] == 'a' || id1[19] == 'b');
    TEST_ASSERT_EQUAL_STRING("", st.token);
    TEST_ASSERT_EQUAL_STRING("", st.pending_href);
    TEST_ESP_OK(espos_sk_store_load(&st, id2));
    TEST_ASSERT_EQUAL_STRING(id1, id2);
    /* "reboot" (re-init the partition) keeps it */
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
    TEST_ESP_OK(espos_sk_store_load(&st, id2));
    TEST_ASSERT_EQUAL_STRING(id1, id2);
    /* a fresh device gets a different one */
    fresh();
    TEST_ESP_OK(espos_sk_store_load(&st, id2));
    TEST_ASSERT_TRUE(strcmp(id1, id2) != 0);
}

TEST_CASE("store: token and pending request round-trip; empties erase", "[sk_store]")
{
    fresh();
    espos_sk_tok_store_t st = { 0 }, back;
    char id[40];
    strcpy(st.token, "eyJhbGciOiJIUzI1NiJ9.some.token");
    strcpy(st.token_self, "urn:mrn:signalk:uuid:aaaa");
    strcpy(st.token_host, "10.0.0.10");
    st.token_port = 80;
    strcpy(st.pending_href, "/signalk/v1/requests/abc");
    strcpy(st.pending_host, "10.0.0.10");
    strcpy(st.pending_self, "urn:mrn:signalk:uuid:aaaa");
    st.pending_port = 3000;
    TEST_ESP_OK(espos_sk_store_save(&st));
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
    TEST_ESP_OK(espos_sk_store_load(&back, id));
    TEST_ASSERT_EQUAL_STRING(st.token, back.token);
    TEST_ASSERT_EQUAL_STRING(st.token_self, back.token_self);
    TEST_ASSERT_EQUAL_STRING(st.token_host, back.token_host);
    TEST_ASSERT_EQUAL(80, back.token_port);
    TEST_ASSERT_EQUAL_STRING(st.pending_href, back.pending_href);
    TEST_ASSERT_EQUAL_STRING(st.pending_host, back.pending_host);
    TEST_ASSERT_EQUAL_STRING(st.pending_self, back.pending_self);
    TEST_ASSERT_EQUAL(3000, back.pending_port);
    /* clearing the token erases the keys, pending untouched */
    st.token[0] = '\0';
    st.token_self[0] = '\0';
    st.token_host[0] = '\0';
    st.token_port = 0;
    TEST_ESP_OK(espos_sk_store_save(&st));
    TEST_ESP_OK(espos_sk_store_load(&back, id));
    TEST_ASSERT_EQUAL_STRING("", back.token);
    TEST_ASSERT_EQUAL_STRING("", back.token_host);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", back.pending_href);
    /* a token rewritten by firmware that predates the address (rollback)
     * does not inherit the old token's address */
    strcpy(st.token, "tok.for.A");
    strcpy(st.token_host, "10.0.0.10");
    st.token_port = 80;
    TEST_ESP_OK(espos_sk_store_save(&st));
    nvs_handle_t h;
    TEST_ESP_OK(nvs_open_from_partition("nvs", "skstate", NVS_READWRITE, &h));
    TEST_ESP_OK(nvs_set_str(h, "token", "tok.for.B"));
    TEST_ESP_OK(nvs_commit(h));
    nvs_close(h);
    TEST_ESP_OK(espos_sk_store_load(&back, id));
    TEST_ASSERT_EQUAL_STRING("tok.for.B", back.token);
    TEST_ASSERT_EQUAL_STRING("", back.token_host);
    TEST_ASSERT_EQUAL(0, back.token_port);
    /* a token at the maximum length fits */
    memset(st.token, 'x', ESPOS_SK_TOKEN_MAX - 1);
    st.token[ESPOS_SK_TOKEN_MAX - 1] = '\0';
    TEST_ESP_OK(espos_sk_store_save(&st));
    TEST_ESP_OK(espos_sk_store_load(&back, id));
    TEST_ASSERT_EQUAL(ESPOS_SK_TOKEN_MAX - 1, strlen(back.token));
}
