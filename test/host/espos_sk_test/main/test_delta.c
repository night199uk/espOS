/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "espos_sk_delta.h"

static espos_sk_delta_t *mk(size_t max_msgs, size_t max_bytes, uint32_t drain)
{
    espos_sk_delta_cfg_t c = { .label = "espos-1a2b", .batch_ms = 100, .max_msgs = max_msgs, .max_bytes = max_bytes, .drain_per_s = drain };
    espos_sk_delta_t *d = espos_sk_delta_create(&c);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

TEST_CASE("batching: values within the window become one delta, last value per path wins", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.speedApparent", "3.5", 1000));
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.angleApparent", "0.7", 1020));
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.speedApparent", "3.6", 1050)); /* replaces */
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 1090, true));                                   /* window open */
    TEST_ASSERT_EQUAL_UINT32(10, espos_sk_delta_next_due_ms(d, 1090, true));
    char *m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING(
        "{\"context\":\"vessels.self\",\"updates\":[{\"source\":{\"label\":\"espos-1a2b\"},\"values\":["
        "{\"path\":\"environment.wind.speedApparent\",\"value\":3.6},"
        "{\"path\":\"environment.wind.angleApparent\",\"value\":0.7}]}]}",
        m);
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 1200, true));
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.built);
    TEST_ASSERT_EQUAL(1, st.taken);
    TEST_ASSERT_EQUAL(0, st.buffered);
    espos_sk_delta_destroy(d);
}

TEST_CASE("offline: messages buffer, oldest dropped at capacity, drained in order and rate-limited", "[delta]")
{
    espos_sk_delta_t *d = mk(3, 0, 10); /* 3 messages, 10/s */
    for (int i = 0; i < 5; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", v, 1000 + i * 1000));
        TEST_ASSERT_NULL(espos_sk_delta_take(d, 1000 + i * 1000 + 100, false)); /* offline: builds, buffers */
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(3, st.buffered);
    TEST_ASSERT_EQUAL(2, st.dropped);                                             /* 0 and 1 gone */
    /* reconnect at t=10000: first message immediately, then ≥100 ms apart */
    char *m = espos_sk_delta_take(d, 10000, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":2}"));
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 10050, true));                        /* too soon */
    TEST_ASSERT_EQUAL_UINT32(50, espos_sk_delta_next_due_ms(d, 10050, true));
    m = espos_sk_delta_take(d, 10100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":3}"));
    free(m);
    m = espos_sk_delta_take(d, 10200, true);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":4}"));
    free(m);
    TEST_ASSERT_NULL(espos_sk_delta_take(d, 10300, true));
    /* live traffic after the backlog is not throttled */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "5", 10300));
    m = espos_sk_delta_take(d, 10400, true);
    TEST_ASSERT_NOT_NULL(m);
    free(m);
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "6", 10401));
    m = espos_sk_delta_take(d, 10501, true);
    TEST_ASSERT_NOT_NULL(m);                                                       /* 101 ms later, no gap needed */
    free(m);
    espos_sk_delta_destroy(d);
}

TEST_CASE("byte cap, requeue after a failed send, forced flush", "[delta]")
{
    espos_sk_delta_t *d = mk(100, 400, 20);
    for (int i = 0; i < 6; i++) {
        TEST_ESP_OK(espos_sk_delta_publish(d, "some.path", "1234567890", 1000 + i * 200));
        espos_sk_delta_flush(d, 0);
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_TRUE(st.buffered_bytes <= 400);
    TEST_ASSERT_TRUE(st.buffered < 6);
    TEST_ASSERT_TRUE(st.dropped > 0);
    size_t before = st.buffered;
    char *m = espos_sk_delta_take(d, 5000, true);
    TEST_ASSERT_NOT_NULL(m);
    espos_sk_delta_requeue(d, m);                                                  /* send failed */
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(before, st.buffered);
    char *again = espos_sk_delta_take(d, 6000, true);
    TEST_ASSERT_NOT_NULL(again);                                                   /* same one first */
    free(again);
    /* an oversized single message is dropped, not buffered */
    char big[ESPOS_SK_VALUE_MAX];
    memset(big, '9', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    for (int i = 0; i < 3; i++) {
        char p[32];
        snprintf(p, sizeof(p), "p.%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, p, big, 7000));
    }
    espos_sk_delta_flush(d, 7000);                                                 /* > 400 bytes */
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_TRUE(st.buffered_bytes <= 400);
    espos_sk_delta_destroy(d);
}

TEST_CASE("publish validation and pending overflow", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish(d, "", "1", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish(d, "a", "", 0));
    char longp[ESPOS_SK_PATH_MAX + 4];
    memset(longp, 'a', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_sk_delta_publish(d, longp, "1", 0));
    for (int i = 0; i < ESPOS_SK_PENDING_MAX + 2; i++) {
        char p[32];
        snprintf(p, sizeof(p), "path.%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, p, "1", 100));
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.built);                                                /* window closed early */
    TEST_ASSERT_EQUAL(2, st.pending);
    espos_sk_delta_destroy(d);
}

#define DINGHY "vessels.urn:mrn:signalk:uuid:6f1c2a3e-1b7d-4c55-9a0e-2d4f8b9c0a11"

TEST_CASE("contexts: one window sends one delta per context, self untouched", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ESP_OK(espos_sk_delta_publish(d, "environment.depth.belowTransducer", "4.2", 1000));
    TEST_ESP_OK(espos_sk_delta_publish_ctx(d, DINGHY, "environment.depth.belowTransducer", "1.5", 1010));
    TEST_ESP_OK(espos_sk_delta_publish_ctx(d, DINGHY, "environment.depth.belowTransducer", "1.6", 1020)); /* replaces */
    TEST_ESP_OK(espos_sk_delta_publish_ctx(d, "vessels.self", "navigation.speedOverGround", "2", 1030));
    char *m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING(
        "{\"context\":\"vessels.self\",\"updates\":[{\"source\":{\"label\":\"espos-1a2b\"},\"values\":["
        "{\"path\":\"environment.depth.belowTransducer\",\"value\":4.2},"
        "{\"path\":\"navigation.speedOverGround\",\"value\":2}]}]}",
        m);
    free(m);
    m = espos_sk_delta_take(d, 1200, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING(
        "{\"context\":\"" DINGHY "\",\"updates\":[{\"source\":{\"label\":\"espos-1a2b\"},\"values\":["
        "{\"path\":\"environment.depth.belowTransducer\",\"value\":1.6}]}]}",
        m);
    free(m);
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(2, st.built);
    espos_sk_delta_destroy(d);
}

TEST_CASE("contexts: a fifth context closes the window early, nothing is lost", "[delta]")
{
    espos_sk_delta_t *d = mk(16, 0, 1000);
    for (int i = 0; i < ESPOS_SK_CONTEXTS_MAX + 1; i++) {
        char c[48];
        snprintf(c, sizeof(c), "vessels.urn:mrn:signalk:mmsi:21100000%d", i);
        TEST_ESP_OK(espos_sk_delta_publish_ctx(d, c, "a.b", "1", 1000));
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(ESPOS_SK_CONTEXTS_MAX, st.built);
    TEST_ASSERT_EQUAL(1, st.pending);
    espos_sk_delta_flush(d, 1000);
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(ESPOS_SK_CONTEXTS_MAX + 1, st.built);
    for (int i = 0; i < ESPOS_SK_CONTEXTS_MAX + 1; i++) {
        char *m = espos_sk_delta_take(d, 2000 + i * 10, true);
        TEST_ASSERT_NOT_NULL(m);
        char c[48];
        snprintf(c, sizeof(c), "\"vessels.urn:mrn:signalk:mmsi:21100000%d\"", i);
        TEST_ASSERT_NOT_NULL(strstr(m, c));
        free(m);
    }
    espos_sk_delta_destroy(d);
}

TEST_CASE("contexts: validation keeps the context from breaking the JSON", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish_ctx(d, "", "a", "1", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish_ctx(d, "vessels.\"x", "a", "1", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish_ctx(d, "vessels.\\x", "a", "1", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sk_delta_publish_ctx(d, "vessels.\nx", "a", "1", 0));
    char longc[ESPOS_SK_CONTEXT_MAX + 1];
    memset(longc, 'v', sizeof(longc) - 1);
    longc[sizeof(longc) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_sk_delta_publish_ctx(d, longc, "a", "1", 0));
    longc[ESPOS_SK_CONTEXT_MAX - 1] = '\0';
    TEST_ESP_OK(espos_sk_delta_publish_ctx(d, longc, "a", "1", 0));
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(1, st.pending);
    espos_sk_delta_destroy(d);
}

TEST_CASE("json helpers", "[delta]")
{
    char b[64];
    espos_sk_json_number(b, sizeof(b), 0.1);
    TEST_ASSERT_EQUAL_STRING("0.1", b);
    espos_sk_json_number(b, sizeof(b), 3.0);
    TEST_ASSERT_EQUAL_STRING("3", b);
    espos_sk_json_number(b, sizeof(b), (double)0.1f);
    TEST_ASSERT_EQUAL_STRING("0.1", b);
    espos_sk_json_number(b, sizeof(b), 1e21);
    TEST_ASSERT_EQUAL_STRING("1e+21", b);
    espos_sk_json_number(b, sizeof(b), 0.0 / 0.0);
    TEST_ASSERT_EQUAL_STRING("null", b);
    espos_sk_json_string(b, sizeof(b), "he said \"hi\"\n");
    TEST_ASSERT_EQUAL_STRING("\"he said \\\"hi\\\"\\u000a\"", b);
}

/* ------------------------------------------------------- timestamps */

/* A settable wall clock, the shape espos_sk_delta_set_clock() takes. `now` of
 * 0 is the honest "this device does not know the time". */
static int64_t s_wall_ms;

static int64_t fake_wall(void *arg)
{
    (void)arg;
    return s_wall_ms;
}

TEST_CASE("timestamps: none until a clock is set, then ISO 8601 UTC with ms", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    /* No clock at all: the message goes out exactly as it did before, and the
     * server stamps it on arrival. */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "1", 1000));
    char *m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NULL(strstr(m, "timestamp"));
    free(m);

    /* A clock that says "I do not know" is the same as no clock. */
    espos_sk_delta_set_clock(d, fake_wall, NULL);
    s_wall_ms = 0;
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "2", 2000));
    m = espos_sk_delta_take(d, 2100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NULL(strstr(m, "timestamp"));
    free(m);

    /* The batch closes 100 ms after the value is published, so the clock at
     * take() time reads 100 ms later than the instant the value belongs to —
     * and the stamp must be the earlier one. */
    s_wall_ms = 1788775933456LL + 100; /* 2026-09-07T10:12:13.556Z now */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "3", 3000));
    m = espos_sk_delta_take(d, 3100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:12:13.456Z\""));
    /* It belongs to the update object, before the source, and the values are
     * still there behind it. */
    TEST_ASSERT_NOT_NULL(strstr(m, "\"updates\":[{\"timestamp\":\"2026-09-07T10:12:13.456Z\",\"source\":"));
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":3}"));
    free(m);
    espos_sk_delta_destroy(d);
}

TEST_CASE("timestamps: a message buffered offline keeps its own time after a late sync", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 100);
    espos_sk_delta_set_clock(d, fake_wall, NULL);
    s_wall_ms = 0; /* the device boots with no idea what time it is */

    /* Three measurements, one second apart, while the server is unreachable. */
    for (int i = 0; i < 3; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", i);
        TEST_ESP_OK(espos_sk_delta_publish(d, "environment.wind.speedApparent", v, 10000 + i * 1000));
        TEST_ASSERT_NULL(espos_sk_delta_take(d, 10000 + i * 1000 + 100, false));
    }
    espos_sk_delta_stats_t st;
    espos_sk_delta_stats(d, &st);
    TEST_ASSERT_EQUAL(3, st.buffered);

    /* An hour later the network comes back and SNTP sets the clock. The
     * monotonic counter is at 3610000; the wall clock says 2026-09-07T11:00:00Z.
     * The first message batched at mono 10000, i.e. 3600 s = one hour earlier,
     * so it must be stamped 10:00:00 — NOT the time it is being sent. */
    s_wall_ms = 1788778800000LL; /* 2026-09-07T11:00:00.000Z */
    char *m = espos_sk_delta_take(d, 3610000, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":0}"));
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:00:00.000Z\""));
    free(m);

    /* The second was measured one second after the first and is stamped so,
     * even though it is drained 10 ms later in wall-clock terms. */
    s_wall_ms += 10;
    m = espos_sk_delta_take(d, 3610010, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":1}"));
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:00:01.000Z\""));
    free(m);

    /* Monotonic order is preserved: the third is a further second on. */
    s_wall_ms += 10;
    m = espos_sk_delta_take(d, 3610020, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"value\":2}"));
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:00:02.000Z\""));
    free(m);
    espos_sk_delta_destroy(d);
}

TEST_CASE("timestamps: a requeued message is not stamped twice", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    espos_sk_delta_set_clock(d, fake_wall, NULL);
    s_wall_ms = 1788775933456LL + 100; /* the batch window closes 100 ms on */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "1", 1000));
    char *m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:12:13.456Z\""));
    espos_sk_delta_requeue(d, m); /* the send failed */

    /* Ten seconds later it goes out again. It carries the time it was
     * MEASURED, once, not the time of either send attempt. */
    s_wall_ms += 10000;
    char *again = espos_sk_delta_take(d, 11100, true);
    TEST_ASSERT_NOT_NULL(again);
    TEST_ASSERT_NOT_NULL(strstr(again, "\"timestamp\":\"2026-09-07T10:12:13.456Z\""));
    TEST_ASSERT_NULL(strstr(again, "\"timestamp\":\"2026-09-07T10:12:23.456Z\""));
    /* Exactly one timestamp member in the document. */
    const char *first = strstr(again, "\"timestamp\"");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "\"timestamp\""));
    free(again);
    espos_sk_delta_destroy(d);
}

TEST_CASE("timestamps: the clock can be turned off again, and the monotonic wrap is handled", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 20);
    espos_sk_delta_set_clock(d, fake_wall, NULL);
    s_wall_ms = 1788775933456LL;
    /* Batched just before the 32-bit monotonic counter wraps, taken just
     * after: the unsigned difference is 100 ms, not 4.29 billion. */
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "1", 0xffffff9cu)); /* -100 */
    char *m = espos_sk_delta_take(d, 0u, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"timestamp\":\"2026-09-07T10:12:13.356Z\""));
    free(m);

    espos_sk_delta_set_clock(d, NULL, NULL);
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "2", 1000));
    m = espos_sk_delta_take(d, 1100, true);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NULL(strstr(m, "timestamp"));
    free(m);
    espos_sk_delta_destroy(d);
}

TEST_CASE("contexts: every message of a window gets the window's timestamp", "[delta]")
{
    espos_sk_delta_t *d = mk(8, 0, 1000);
    s_wall_ms = 1789000000000LL;
    espos_sk_delta_set_clock(d, fake_wall, NULL);
    TEST_ESP_OK(espos_sk_delta_publish(d, "a.b", "1", 1000));
    TEST_ESP_OK(espos_sk_delta_publish_ctx(d, DINGHY, "a.b", "2", 1050));
    char *m1 = espos_sk_delta_take(d, 1100, true);
    s_wall_ms += 10; /* the clock moves on with the monotonic one */
    char *m2 = espos_sk_delta_take(d, 1110, true);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_NOT_NULL(m2);
    const char *t1 = strstr(m1, "\"timestamp\":\"");
    const char *t2 = strstr(m2, "\"timestamp\":\"");
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_EQUAL_STRING_LEN(t1, t2, 39);
    free(m1);
    free(m2);
    espos_sk_delta_destroy(d);
}
