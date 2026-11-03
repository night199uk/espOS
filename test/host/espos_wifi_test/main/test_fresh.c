// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
/*
 * wifi.fresh_assoc (espOS #289): drop the first association of a boot once.
 * A wrong answer either drops every association or reconnects behind the
 * state machine's back.
 */

#include "unity.h"
#include "wifi_fresh.h"

TEST_CASE("the first association is dropped once and the echo reconnects", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("disabled at the first association means never for this boot", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, false));
    /* turned on later: the AP already has a fresh association, nothing to drop */
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("a disconnect before any association is not swallowed", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("another reason while pending goes to the state machine", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, false));
    /* and the drop is over: a later leave is the state machine's own */
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("the state machine taking over cancels a pending drop", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    espos_wifi_fresh_cancel(&f);
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("cancel before the first association keeps it armed", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    espos_wifi_fresh_cancel(&f);
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("an association while pending is reported, not dropped again", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
}

TEST_CASE("armed and pending follow the drop", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_armed(&f));
    TEST_ASSERT_FALSE(espos_wifi_fresh_pending(&f));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_armed(&f));
    TEST_ASSERT_TRUE(espos_wifi_fresh_pending(&f));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_armed(&f));
    /* still pending, so the port withholds a late address of the dropped association */
    TEST_ASSERT_TRUE(espos_wifi_fresh_pending(&f));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_pending(&f));
}

TEST_CASE("a failed rejoin after the echo goes to the state machine", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_disconnected(&f, true));
    /* a second leave is the state machine's own, not another echo */
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_disconnected(&f, true));
    TEST_ASSERT_FALSE(espos_wifi_fresh_pending(&f));
    TEST_ASSERT_FALSE(espos_wifi_fresh_on_connected(&f, true));
}

TEST_CASE("cancel after the echo ends the drop", "[fresh]")
{
    espos_wifi_fresh_t f = { 0 };
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_connected(&f, true));
    TEST_ASSERT_TRUE(espos_wifi_fresh_on_disconnected(&f, true));
    espos_wifi_fresh_cancel(&f);
    TEST_ASSERT_FALSE(espos_wifi_fresh_pending(&f));
}
