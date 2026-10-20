/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "espos_sk_delta.h"

/* main/CMakeLists.txt builds this project with CONFIG_ESPOS_SK_PENDING_MAX=4,
 * so the window closes after four values rather than the public default of 32.
 * Keep CAP in step with that definition: if they drift, this test fails. */
#define CAP 4

static espos_sk_delta_t *mk(void)
{
    espos_sk_delta_cfg_t c = { .label = "espos-1a2b", .batch_ms = 100, .max_msgs = 8, .max_bytes = 0, .drain_per_s = 20 };
    espos_sk_delta_t *d = espos_sk_delta_create(&c);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

TEST_CASE("pending: the configured cap closes the window and keeps the next value", "[delta]")
{
    espos_sk_delta_t *d = mk();
    for (int i = 0; i < CAP; i++) {
        char path[16];
        char value[8];
        snprintf(path, sizeof(path), "a.%d", i);
        snprintf(value, sizeof(value), "%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, path, value, 1000 + i));
    }

    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(CAP, st.pending); /* the window is full and nothing is sent */
    TEST_ASSERT_EQUAL(0, st.built);

    /* One value beyond the cap closes the full window into a delta and starts
     * the next window; no pending value is discarded. */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.next", "9", 2000));
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.pending);
    TEST_ASSERT_EQUAL(1, st.built);

    /* At 2000 the second window (opened at 2000) is not yet due, so take()
     * only hands out the closed window; the new value stays pending. */
    char *m = espos_sk_delta_take(d, 2000, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":3}"));
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 2000, true));
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.pending);

    espos_sk_delta_destroy(d);
}
