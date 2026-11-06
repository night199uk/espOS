/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Server choice with two servers advertising the same self URN (a test VM
 * configured with the boat's MMSI): the anchored one must win by address in
 * whatever order mDNS answers, and the choice must not flip between rounds.
 */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "espos_sk_priv.h"

#define TWIN_SELF "urn:mrn:imo:mmsi:211161020"

static espos_sk_discovered_t disc(const char *host, uint16_t port, const char *self)
{
    espos_sk_discovered_t d = { .port = port };
    strcpy(d.host, host);
    strcpy(d.self, self);
    return d;
}

TEST_CASE("select: the anchor's address wins over a twin with the same self, in either order", "[sk_select]")
{
    espos_sk_tok_store_t st = { .pending_href = "/signalk/v1/requests/abc", .pending_host = "192.168.0.10", .pending_port = 80, .pending_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t ab[2] = { disc("192.168.0.10", 80, TWIN_SELF), disc("192.168.0.122", 80, TWIN_SELF) };
    espos_sk_discovered_t ba[2] = { ab[1], ab[0] };
    /* even with the twin as the current server */
    TEST_ASSERT_EQUAL(0, espos_sk_find_anchored(ab, 2, &a, "192.168.0.122", 80, false));
    TEST_ASSERT_EQUAL(1, espos_sk_find_anchored(ba, 2, &a, "192.168.0.122", 80, false));

    /* a token outranks the pending request, and anchors by its own address */
    strcpy(st.token, "tok.en");
    strcpy(st.token_self, TWIN_SELF);
    strcpy(st.token_host, "192.168.0.122");
    st.token_port = 80;
    espos_sk_store_anchor(&st, &a);
    TEST_ASSERT_EQUAL(1, espos_sk_find_anchored(ab, 2, &a, "", 0, false));
    TEST_ASSERT_EQUAL(0, espos_sk_find_anchored(ba, 2, &a, "", 0, false));
}

TEST_CASE("select: by self -- only without an address or once it has moved; current server first, else lowest address", "[sk_select]")
{
    /* a token from before the address was recorded */
    espos_sk_tok_store_t st = { .token = "tok.en", .token_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t ab[3] = { disc("10.0.0.5", 3000, "urn:mrn:signalk:uuid:other"),
                                    disc("192.168.0.122", 80, TWIN_SELF), disc("192.168.0.10", 80, TWIN_SELF) };
    espos_sk_discovered_t ba[3] = { ab[0], ab[2], ab[1] };
    /* no current server: deterministic by address, not by answer order */
    TEST_ASSERT_EQUAL_STRING("192.168.0.10", ab[espos_sk_find_anchored(ab, 3, &a, "", 0, false)].host);
    TEST_ASSERT_EQUAL_STRING("192.168.0.10", ba[espos_sk_find_anchored(ba, 3, &a, "", 0, false)].host);
    /* the one in use stays in use */
    TEST_ASSERT_EQUAL_STRING("192.168.0.122", ab[espos_sk_find_anchored(ab, 3, &a, "192.168.0.122", 80, false)].host);
    TEST_ASSERT_EQUAL_STRING("192.168.0.122", ba[espos_sk_find_anchored(ba, 3, &a, "192.168.0.122", 80, false)].host);
    /* recorded address silent (first round after boot, a lossy one): the
     * same-self entries are twins until the caller says it has moved... */
    strcpy(st.token_host, "192.168.0.99");
    st.token_port = 80;
    espos_sk_store_anchor(&st, &a);
    TEST_ASSERT_EQUAL(-1, espos_sk_find_anchored(ab, 3, &a, "", 0, false));
    /* ...and once it has (DHCP), self finds it */
    TEST_ASSERT_EQUAL_STRING(TWIN_SELF, ab[espos_sk_find_anchored(ab, 3, &a, "", 0, true)].self);
}

TEST_CASE("select: an address handed to another server is not the anchor", "[sk_select]")
{
    espos_sk_tok_store_t st = { .token = "tok.en", .token_self = TWIN_SELF, .token_host = "192.168.0.10", .token_port = 80 };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t l[2] = { disc("192.168.0.10", 80, "urn:mrn:imo:mmsi:999999999"), disc("192.168.0.20", 80, TWIN_SELF) };
    TEST_ASSERT_FALSE(espos_sk_is_anchor(&l[0], &a));
    TEST_ASSERT_EQUAL(-1, espos_sk_find_anchored(l, 2, &a, "", 0, false));
    TEST_ASSERT_EQUAL(1, espos_sk_find_anchored(l, 2, &a, "", 0, true));
    /* an entry that advertised no self is not a contradiction */
    espos_sk_discovered_t noself = disc("192.168.0.10", 80, "");
    TEST_ASSERT_TRUE(espos_sk_is_anchor(&noself, &a));
}

TEST_CASE("select: nothing stored anchors nothing", "[sk_select]")
{
    espos_sk_tok_store_t st = { 0 };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t l[1] = { disc("192.168.0.10", 80, TWIN_SELF) };
    TEST_ASSERT_EQUAL(-1, espos_sk_find_anchored(l, 1, &a, "", 0, false));
    /* a stale pending address with no href is not an anchor either */
    strcpy(st.pending_host, "192.168.0.10");
    st.pending_port = 80;
    espos_sk_store_anchor(&st, &a);
    TEST_ASSERT_EQUAL(-1, espos_sk_find_anchored(l, 1, &a, "", 0, false));
}

TEST_CASE("select: election order is self, then current server, then address", "[sk_select]")
{
    espos_sk_discovered_t a = disc("192.168.0.122", 80, TWIN_SELF);
    espos_sk_discovered_t b = disc("192.168.0.10", 80, TWIN_SELF);
    espos_sk_discovered_t c = disc("192.168.0.200", 80, "urn:mrn:imo:mmsi:100000000");
    TEST_ASSERT_TRUE(espos_sk_disc_before(&c, &a, "192.168.0.122", 80));  /* lower self wins outright */
    TEST_ASSERT_TRUE(espos_sk_disc_before(&b, &a, "", 0));
    TEST_ASSERT_FALSE(espos_sk_disc_before(&a, &b, "", 0));
    TEST_ASSERT_TRUE(espos_sk_disc_before(&a, &b, "192.168.0.122", 80));
    TEST_ASSERT_FALSE(espos_sk_disc_before(&b, &a, "192.168.0.122", 80));
    espos_sk_discovered_t d = disc("192.168.0.10", 3000, TWIN_SELF);
    TEST_ASSERT_TRUE(espos_sk_disc_before(&b, &d, "", 0));
    TEST_ASSERT_FALSE(espos_sk_disc_before(&b, &b, "", 0));               /* strict */
}

/* Twelve plaintext answers fill a list of twelve before the https pass runs.
 * The paired TLS server must still get in; anything else must not. */
TEST_CASE("disc_add: a full list still takes the anchored server, once, in the last slot", "[sk_select]")
{
    enum { MAX = 12 };
    espos_sk_discovered_t out[MAX];
    size_t n = 0;
    espos_sk_tok_store_t st = { .token = "t", .token_host = "10.0.0.99", .token_port = 3443, .token_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    const espos_sk_keep_t keep = { .anchor = &a, .cur_host = "", .cur_port = 0, .self_fallback = false };
    for (int i = 0; i < MAX; i++) {
        char host[16];
        snprintf(host, sizeof(host), "10.0.0.%d", i + 1);
        espos_sk_discovered_t d = disc(host, 80, "urn:mrn:imo:mmsi:100000000");
        espos_sk_disc_add(out, &n, MAX, &d, &keep);
    }
    TEST_ASSERT_EQUAL(MAX, n);
    espos_sk_discovered_t other = disc("10.0.0.50", 3443, "urn:mrn:imo:mmsi:100000001");
    other.tls = true;
    espos_sk_disc_add(out, &n, MAX, &other, &keep);
    TEST_ASSERT_EQUAL_STRING("10.0.0.12", out[MAX - 1].host); /* not the anchor: left off */
    espos_sk_discovered_t ours = disc("10.0.0.99", 3443, TWIN_SELF);
    ours.tls = true;
    espos_sk_disc_add(out, &n, MAX, &ours, &keep);
    TEST_ASSERT_EQUAL(MAX, n);
    TEST_ASSERT_EQUAL_STRING("10.0.0.99", out[MAX - 1].host);
    TEST_ASSERT_TRUE(out[MAX - 1].tls);
    espos_sk_discovered_t again = disc("10.0.0.11", 80, "urn:mrn:imo:mmsi:100000000");
    espos_sk_disc_add(out, &n, MAX, &again, &keep); /* a duplicate is replaced in place */
    TEST_ASSERT_EQUAL_STRING("10.0.0.99", out[MAX - 1].host);
    TEST_ASSERT_EQUAL(MAX, n);
}

TEST_CASE("disc_add: a later answer for the same address replaces it (https after http)", "[sk_select]")
{
    espos_sk_discovered_t out[2];
    size_t n = 0;
    espos_sk_discovered_t plain = disc("10.0.0.10", 3000, TWIN_SELF);
    espos_sk_discovered_t tls = plain;
    tls.tls = true;
    espos_sk_disc_add(out, &n, 2, &plain, NULL);
    espos_sk_disc_add(out, &n, 2, &tls, NULL);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_TRUE(out[0].tls);
}

static size_t merge(const espos_sk_discovered_t *table, const uint32_t *table_avoid, size_t table_n,
                    const espos_sk_discovered_t *found, size_t found_n, const espos_sk_anchor_t *a, uint32_t now,
                    espos_sk_discovered_t *out, uint32_t *out_avoid, size_t cap)
{
    const espos_sk_merge_in_t in = {
        .table = table,
        .table_avoid = table_avoid,
        .table_n = table_n,
        .found = found,
        .found_n = found_n,
        .anchor = a,
        .cur_host = "",
        .cur_port = 0,
        .self_fallback = false,
        .now_ms = now,
        .carry_ms = 125000,
    };
    return espos_sk_merge_servers(&in, out, out_avoid, cap);
}

TEST_CASE("merge: a one-server table on a fresh boot keeps the paired https server", "[sk_select]")
{
    espos_sk_tok_store_t st = { .token = "t", .token_host = "10.0.0.99", .token_port = 3443, .token_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t found[12];
    for (int i = 0; i < 11; i++) {
        char host[16];
        snprintf(host, sizeof(host), "10.0.0.%d", i + 1);
        found[i] = disc(host, 80, "urn:mrn:imo:mmsi:100000000");
    }
    found[11] = disc("10.0.0.99", 3443, TWIN_SELF);
    found[11].tls = true;
    espos_sk_discovered_t out[1];
    uint32_t avoid[1] = { 7 };
    size_t m = merge(NULL, NULL, 0, found, 12, &a, 1000, out, avoid, 1);
    TEST_ASSERT_EQUAL(1, m);
    TEST_ASSERT_EQUAL_STRING("10.0.0.99", out[0].host);
    TEST_ASSERT_TRUE(out[0].tls);
    TEST_ASSERT_EQUAL_UINT32(1000, out[0].seen_ms);
    TEST_ASSERT_EQUAL_UINT32(0, avoid[0]);
}

TEST_CASE("merge: anchor first, then answers, then carried-over entries; stale ones and stamps", "[sk_select]")
{
    espos_sk_tok_store_t st = { .token = "t", .token_host = "10.0.0.20", .token_port = 80, .token_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t table[3] = { disc("10.0.0.30", 80, "urn:x:c"), disc("10.0.0.20", 80, TWIN_SELF),
                                       disc("10.0.0.40", 80, "urn:x:d") };
    table[0].seen_ms = 90000; /* carried */
    table[1].seen_ms = 90000; /* the anchor, unanswered this round but fresh */
    table[2].seen_ms = 0;     /* older than the carry window: dropped */
    const uint32_t table_avoid[3] = { 11, 22, 33 };
    espos_sk_discovered_t found[2] = { disc("10.0.0.50", 80, "urn:x:e"), disc("10.0.0.30", 80, "urn:x:c") };
    espos_sk_discovered_t out[4];
    uint32_t avoid[4];
    size_t m = merge(table, table_avoid, 3, found, 2, &a, 200000, out, avoid, 4);
    TEST_ASSERT_EQUAL(3, m);
    TEST_ASSERT_EQUAL_STRING("10.0.0.20", out[0].host);
    TEST_ASSERT_EQUAL_UINT32(90000, out[0].seen_ms); /* carried, not refreshed */
    TEST_ASSERT_EQUAL_UINT32(22, avoid[0]);
    TEST_ASSERT_EQUAL_STRING("10.0.0.50", out[1].host);
    TEST_ASSERT_EQUAL_UINT32(0, avoid[1]);
    TEST_ASSERT_EQUAL_STRING("10.0.0.30", out[2].host);
    TEST_ASSERT_EQUAL_UINT32(200000, out[2].seen_ms); /* answered: refreshed */
    TEST_ASSERT_EQUAL_UINT32(11, avoid[2]);           /* its avoid stamp carried over */
    TEST_ASSERT_EQUAL_UINT32(0, avoid[3]);
    /* a cap of two keeps the anchor and the first answer */
    m = merge(table, table_avoid, 3, found, 2, &a, 200000, out, avoid, 2);
    TEST_ASSERT_EQUAL(2, m);
    TEST_ASSERT_EQUAL_STRING("10.0.0.20", out[0].host);
    TEST_ASSERT_EQUAL_STRING("10.0.0.50", out[1].host);
}

TEST_CASE("disc_add: a full list follows a moved server by self, and keeps the server in use over a twin", "[sk_select]")
{
    espos_sk_tok_store_t st = { .token = "t", .token_host = "10.0.0.99", .token_port = 80, .token_self = TWIN_SELF };
    espos_sk_anchor_t a;
    espos_sk_store_anchor(&st, &a);
    espos_sk_discovered_t out[2] = { disc("10.0.0.1", 80, "urn:x:a"), disc("10.0.0.2", 80, "urn:x:b") };
    size_t n = 2;
    /* Not moved yet: an answer with our self at another address stays out. */
    const espos_sk_keep_t strict = { .anchor = &a, .cur_host = "10.0.0.20", .cur_port = 80, .self_fallback = false };
    espos_sk_discovered_t twin = disc("10.0.0.30", 80, TWIN_SELF);
    espos_sk_disc_add(out, &n, 2, &twin, &strict);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", out[1].host);
    /* Moved: found by self, and the server in use beats the twin whichever
     * answers first. */
    const espos_sk_keep_t moved = { .anchor = &a, .cur_host = "10.0.0.20", .cur_port = 80, .self_fallback = true };
    espos_sk_disc_add(out, &n, 2, &twin, &moved);
    TEST_ASSERT_EQUAL_STRING("10.0.0.30", out[1].host);
    espos_sk_discovered_t in_use = disc("10.0.0.20", 80, TWIN_SELF);
    espos_sk_disc_add(out, &n, 2, &in_use, &moved);
    TEST_ASSERT_EQUAL_STRING("10.0.0.20", out[1].host);
    espos_sk_disc_add(out, &n, 2, &twin, &moved); /* the twin again: no flip back */
    TEST_ASSERT_EQUAL_STRING("10.0.0.20", out[1].host);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", out[0].host);
    TEST_ASSERT_EQUAL(2, n);
}
