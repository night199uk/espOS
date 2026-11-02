/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * POST scheduling. The gateway used to POST once per interval, which capped
 * it at one batch per ble.post_int_ms and shed advertisements on a busy boat
 * while the link sat idle.
 */
#include "ble_proto.h"
#include "unity.h"

#define BATCH       40
#define CAP         500
#define INTERVAL_MS 2000
#define MS          1000LL

TEST_CASE("a fresh schedule is due at once", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 0, 0, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 5 * MS, 1, BATCH, CAP));
}

TEST_CASE("a partial batch waits for the interval", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, true, INTERVAL_MS);
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 100 * MS, BATCH - 1, BATCH, CAP));
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 1999 * MS, BATCH - 1, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 2000 * MS, BATCH - 1, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 2000 * MS, 0, BATCH, CAP));
}

TEST_CASE("a full batch goes out before the interval", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, true, INTERVAL_MS);
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 100 * MS, BATCH, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 100 * MS, 500, BATCH, CAP));
}

TEST_CASE("back to back full batches drain a backlog", "[post]")
{
    /* 500 buffered, one POST per 100 ms pass: all but the last partial
     * batch go out before the first interval ends. */
    espos_ble_post_sched_t s = { 0 };
    size_t pending = 500;
    int64_t now = 0;
    int posts = 0;
    while (now < INTERVAL_MS * MS) {
        if (espos_ble_post_due(&s, now, pending, BATCH, CAP)) {
            pending -= pending < BATCH ? pending : BATCH;
            espos_ble_post_done(&s, now, true, INTERVAL_MS);
            posts++;
        }
        now += 100 * MS;
    }
    TEST_ASSERT_EQUAL_INT(500 / BATCH, posts);
    TEST_ASSERT_EQUAL_UINT(500 % BATCH, pending);
}

TEST_CASE("after a failed POST a full batch waits for the interval", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, false, INTERVAL_MS);
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 100 * MS, 500, BATCH, CAP));
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 1999 * MS, 500, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 2000 * MS, 500, BATCH, CAP));
}

TEST_CASE("a successful POST lifts the hold", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, false, INTERVAL_MS);
    espos_ble_post_done(&s, 2000 * MS, true, INTERVAL_MS);
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 2100 * MS, BATCH, BATCH, CAP));
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 2100 * MS, BATCH - 1, BATCH, CAP));
}

TEST_CASE("a zero batch size never posts early", "[post]")
{
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, true, INTERVAL_MS);
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 100 * MS, 0, 0, CAP));
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 100 * MS, 0, BATCH, 0));
}

TEST_CASE("a ring smaller than a batch posts when it is full", "[post]")
{
    /* ble.max_pend_ads can be 10, and a short heap shrinks the ring further:
     * 40 is never reached, so full means full. */
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 0, true, INTERVAL_MS);
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 100 * MS, 9, BATCH, 10));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 100 * MS, 10, BATCH, 10));
}

TEST_CASE("the hold runs from the end of a slow POST", "[post]")
{
    /* A POST that started at 0 and failed after its 3 s timeout: the retry
     * waits a full interval from 3 s, not from 0. */
    espos_ble_post_sched_t s = { 0 };
    espos_ble_post_done(&s, 3000 * MS, false, INTERVAL_MS);
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 3100 * MS, CAP, BATCH, CAP));
    TEST_ASSERT_FALSE(espos_ble_post_due(&s, 4999 * MS, CAP, BATCH, CAP));
    TEST_ASSERT_TRUE(espos_ble_post_due(&s, 5000 * MS, CAP, BATCH, CAP));
}
