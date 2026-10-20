/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Delta batcher + offline ring buffer — pure C, no OS calls, host-testable.
 *
 *   publish(path, value)  →  pending (last value per path and timestamp wins)
 *   after batch_ms         →  one delta message per context {"context":"vessels.self","updates":[...]},
 *                             an update per timestamp
 *   connected              →  message goes out (backlog drains oldest-first, rate-limited)
 *   offline                →  message enters the ring (bounded by count and bytes, oldest dropped)
 *
 * The owner calls espos_sk_delta_take() from its sender task and transmits
 * what it gets; espos_sk_delta_requeue() puts a message back if the send
 * failed. Everything else is bookkeeping.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_SK_PATH_MAX  96
#define ESPOS_SK_VALUE_MAX 256
/* The cap on values coalesced in one batch window, and the number the API
 * promises. CONFIG_ESPOS_SK_PENDING_MAX tunes the table the implementation
 * actually allocates; reaching a smaller cap closes the window earlier (more,
 * smaller deltas) but does not itself discard a pending value. The resulting
 * delta remains subject to the ring's configured overflow behaviour. */
#define ESPOS_SK_PENDING_MAX 32
/* "vessels.urn:mrn:signalk:uuid:" plus a 36-character UUID is 65. */
#define ESPOS_SK_CONTEXT_MAX 80
/* Distinct contexts one batch window holds before it closes early. */
#define ESPOS_SK_CONTEXTS_MAX 4

typedef struct {
    const char *label;        /* source label, e.g. the hostname */
    uint32_t batch_ms;        /* coalescing window (default 100) */
    size_t max_msgs;          /* ring capacity in messages */
    size_t max_bytes;         /* ring capacity in bytes (sum of message lengths) */
    uint32_t drain_per_s;     /* max backlog messages per second while draining */
    bool no_drop;             /* refuse with ESP_ERR_NO_MEM instead of dropping the oldest */
} espos_sk_delta_cfg_t;

typedef struct {
    size_t pending;           /* values waiting for the batch window */
    size_t buffered;          /* messages in the ring */
    size_t buffered_bytes;
    uint32_t dropped;         /* messages dropped because the ring was full */
    uint32_t built;           /* messages built */
    uint32_t taken;           /* messages handed to the sender */
} espos_sk_delta_stats_t;

typedef struct espos_sk_delta espos_sk_delta_t;

espos_sk_delta_t *espos_sk_delta_create(const espos_sk_delta_cfg_t *cfg);
void espos_sk_delta_destroy(espos_sk_delta_t *d);

/** value_json is a complete JSON value (number, "string", true, {...}). */
esp_err_t espos_sk_delta_publish(espos_sk_delta_t *d, const char *path, const char *value_json, uint32_t now_ms);
/** The same for another context ("vessels.urn:mrn:signalk:uuid:…"); NULL is
 * vessels.self. A window's values go out as one message per context.
 * ESP_ERR_INVALID_SIZE for a context of ESPOS_SK_CONTEXT_MAX or more
 * characters, ESP_ERR_INVALID_ARG for an empty one or one containing a quote,
 * backslash or control character. */
esp_err_t espos_sk_delta_publish_ctx(espos_sk_delta_t *d, const char *context, const char *path,
                                     const char *value_json, uint32_t now_ms);
/** The same, dated `at_ms` (unix ms) rather than when its batch closes; 0 is
 * the plain call. A context's values go out in one message with an update per
 * timestamp, in the order each timestamp was first published, and a dated
 * update is sent whatever the clock set with espos_sk_delta_set_clock().
 * Within a window a path keeps its last value per timestamp. A value that
 * would join an update sent ahead of an older value of its path closes the
 * window instead, so each path's values reach the server in the order they
 * were published; values of different paths may be reordered. A path
 * alternating between dated and undated values therefore closes a window at
 * every change, and a backlog replayed beside live values of the same path
 * fills the ring a message per value.
 * ESP_ERR_INVALID_ARG for an at_ms before 2000 or past the year 9999.
 * ESP_ERR_NO_MEM when no_drop is set and the ring cannot take the window's
 * messages: nothing is enqueued, so the caller can wait and retry. */
esp_err_t espos_sk_delta_publish_at(espos_sk_delta_t *d, const char *context, const char *path,
                                    const char *value_json, int64_t at_ms, uint32_t now_ms);

/** Sender side. Returns a malloc'ed message to transmit now, or NULL.
 * connected=false only batches into the ring. */
char *espos_sk_delta_take(espos_sk_delta_t *d, uint32_t now_ms, bool connected);
/** Put a message back at the head (send failed); takes ownership. */
void espos_sk_delta_requeue(espos_sk_delta_t *d, char *msg);
/** Force the pending batch out now (e.g. before disconnect). ESP_ERR_NO_MEM
 * when no_drop is set and the ring is full: nothing was queued and the pending
 * values are kept. */
esp_err_t espos_sk_delta_flush(espos_sk_delta_t *d, uint32_t now_ms);
/** ms until something is due (batch window / drain gap); UINT32_MAX if idle. */
uint32_t espos_sk_delta_next_due_ms(const espos_sk_delta_t *d, uint32_t now_ms, bool connected);
void espos_sk_delta_stats(const espos_sk_delta_t *d, espos_sk_delta_stats_t *out);
void espos_sk_delta_set_label(espos_sk_delta_t *d, const char *label);
/** Change batching window / drain rate on a live engine. */
void espos_sk_delta_set_timing(espos_sk_delta_t *d, uint32_t batch_ms, uint32_t drain_per_s);

/**
 * Refuse new values with ESP_ERR_NO_MEM when the ring is full rather than
 * dropping the oldest message, so a caller forwarding a backlog it cannot
 * afford to lose can wait for the ring to drain and retry. Off by default:
 * the offline ring is meant to drop its oldest and keep the newest. Turn it
 * on around a replay and off again for live values.
 */
void espos_sk_delta_set_no_drop(espos_sk_delta_t *d, bool no_drop);

/**
 * Give the engine a wall clock, and every message it hands out carries the
 * time its values were actually MEASURED — `updates[].timestamp`, ISO 8601
 * UTC with milliseconds.
 *
 * This matters most exactly where it is hardest: a message built while the
 * server was unreachable sits in the ring for minutes or hours, and without a
 * timestamp the server stamps it on arrival, so an outage's worth of data
 * lands as one burst at reconnect. The engine records the monotonic instant
 * each message was built and, at take() time, converts it with the clock's
 * current offset — `wall_now - (mono_now - batch_ms)`. That is correct even
 * when the clock was set AFTER the message was buffered, which is the normal
 * case for a device that boots without a network.
 *
 * `wall_ms` returns unix milliseconds, or 0 while the device does not know the
 * time; 0 means no timestamp is emitted at all, and the server falls back to
 * stamping on arrival as it did before. Pass NULL to turn timestamps off.
 * `arg` is handed back untouched. Called on the owner's task; the callback
 * runs under whatever lock the owner holds around take(), so it must not
 * block — espos_time_now_ms_or_zero is a lock-protected snapshot copy and is
 * the intended argument.
 */
void espos_sk_delta_set_clock(espos_sk_delta_t *d, int64_t (*wall_ms)(void *arg), void *arg);

/* Helpers: format values as JSON into buf. */
int espos_sk_json_number(char *buf, size_t size, double v);
int espos_sk_json_string(char *buf, size_t size, const char *s);

#ifdef __cplusplus
}
#endif
