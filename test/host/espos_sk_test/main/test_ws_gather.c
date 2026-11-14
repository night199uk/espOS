/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Message boundaries on the stream: esp_transport_ws reports only the current
 * frame's payload length and fin flag, and hands the payload back in pieces.
 * A message must end with the last piece of the frame carrying fin, never
 * earlier, however the server splits it into frames.
 */
#include "unity.h"
#include "espos_sk_priv.h"

TEST_CASE("ws gather: one frame read in pieces ends with its last piece", "[sk_ws]")
{
    espos_sk_ws_gather_t g = { 0 };
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 1000, 2500, true));
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 1000, 2500, true));
    TEST_ASSERT_TRUE(espos_sk_ws_gather_step(&g, 500, 2500, true));
}

TEST_CASE("ws gather: a later fragment read in pieces is not cut short", "[sk_ws]")
{
    /* 1000 bytes without fin, then 1500 with fin arriving as 1024 + 476.
     * Counting all bytes against the current frame's length would end the
     * message after the first 1024 of the second fragment. */
    espos_sk_ws_gather_t g = { 0 };
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 1000, 1000, false));
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 1024, 1500, true));
    TEST_ASSERT_TRUE(espos_sk_ws_gather_step(&g, 476, 1500, true));
}

TEST_CASE("ws gather: the state is ready for the next message", "[sk_ws]")
{
    espos_sk_ws_gather_t g = { 0 };
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 10, 20, true));
    TEST_ASSERT_TRUE(espos_sk_ws_gather_step(&g, 10, 20, true));
    TEST_ASSERT_EQUAL_UINT(0, g.frame_have);
    TEST_ASSERT_TRUE(espos_sk_ws_gather_step(&g, 5, 5, true));
}

TEST_CASE("ws zero read: an empty final continuation ends the message", "[sk_ws]")
{
    /* a non-empty fragment without fin, then an empty continuation with fin */
    espos_sk_ws_gather_t g = { 0 };
    TEST_ASSERT_FALSE(espos_sk_ws_gather_step(&g, 300, 300, false));
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_DONE, espos_sk_ws_zero_read(true, true, 0, true, true));
    /* the same without fin keeps waiting for the next fragment */
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_WAIT, espos_sk_ws_zero_read(true, true, 0, false, true));
}

TEST_CASE("ws zero read: a timeout mid-payload is a cut, before and inside a message", "[sk_ws]")
{
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_CUT, espos_sk_ws_zero_read(true, false, 1500, true, false));
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_CUT, espos_sk_ws_zero_read(true, true, 1500, false, true));
}

TEST_CASE("ws zero read: a control frame or a quiet line is not an end", "[sk_ws]")
{
    /* a ping handled by the transport, or a header read that timed out
     * (opcode NONE): neither is a data frame */
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_IDLE, espos_sk_ws_zero_read(false, false, 0, true, false));
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_WAIT, espos_sk_ws_zero_read(false, false, 0, true, true));
    /* an empty text frame outside a message carries nothing to deliver */
    TEST_ASSERT_EQUAL(ESPOS_SK_WS_ZERO_IDLE, espos_sk_ws_zero_read(true, false, 0, true, false));
}
