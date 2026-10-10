/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Server choice with two servers advertising the same self URN (a test VM
 * configured with the boat's MMSI): the anchored one must win by address in
 * whatever order mDNS answers, and the choice must not flip between rounds.
 */
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
