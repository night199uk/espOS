/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wire-format helpers for signalk-server's BLE provider API. Pure C with no
 * BLE, network or FreeRTOS dependency, so it runs under Unity on the linux
 * host target - which is where the fiddly parts (hex case, defaulting,
 * buffer accounting) are actually verified.
 *
 * The formats are a contract with signalk-server
 * (packages/server-api/src/typebox/ble-schemas.ts). Two asymmetries are
 * deliberate and load-bearing:
 *   - advertisement `adv_data` is UPPERCASE hex, GATT `data` is lowercase;
 *   - an absent `with_response` means write-WITH-response.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hex helpers. Both NUL-terminate and need 2*len+1 bytes of output. */
void espos_ble_hex_encode_upper(const uint8_t *data, size_t len, char *out);
void espos_ble_hex_encode_lower(const uint8_t *data, size_t len, char *out);
/* Decode `hex` into out (max out_max bytes). Returns bytes written, or -1 on
 * a non-hex digit or odd length. */
int espos_ble_hex_decode(const char *hex, uint8_t *out, size_t out_max);

/* Ring buffer of pending advertisements.
 *
 * Fixed capacity with drop-oldest: a server that is slow, unreachable or
 * simply absent must never grow this without bound. Drops are counted, so
 * "we lost data" is visible rather than silent. */
typedef struct {
    espos_ble_adv_t *slots;
    size_t cap;
    size_t head;    /* next write */
    size_t count;   /* live entries */
    uint32_t dropped;
} espos_ble_advq_t;

void espos_ble_advq_init(espos_ble_advq_t *q, espos_ble_adv_t *storage, size_t cap);
/* Append, evicting the oldest when full. Returns true if something was
 * dropped to make room. */
bool espos_ble_advq_push(espos_ble_advq_t *q, const espos_ble_adv_t *adv);
/* Copy up to max entries oldest-first into out and remove them. */
size_t espos_ble_advq_drain(espos_ble_advq_t *q, espos_ble_adv_t *out, size_t max);
static inline size_t espos_ble_advq_count(const espos_ble_advq_t *q) { return q->count; }

/* When the gateway POSTs.
 *
 * A full batch goes out as soon as it is waiting and a partial one after at
 * most the interval. One POST per interval would cap the rate at one batch
 * per ble.post_int_ms (40 per 2 s, 20 a second), fewer than thirty-odd
 * devices in range send. After a failed POST only the interval counts, so a
 * struggling server is not retried on every pass of the gateway task. */
typedef struct {
    int64_t next_us; /* the interval's next deadline */
    bool held;       /* the last POST failed */
} espos_ble_post_sched_t;

/* True when a POST is due at now_us with `pending` advertisements buffered.
 * A batch is full at batch_max, or at `capacity` when the ring holds fewer
 * than that: a ring of 10 never reaches 40. A zeroed schedule is due at
 * once. */
bool espos_ble_post_due(const espos_ble_post_sched_t *s, int64_t now_us, size_t pending, size_t batch_max,
                        size_t capacity);
/* Records a POST that finished at now_us: the next interval deadline is
 * interval_ms on, and a failure holds full batches back until then. The time
 * is the end of the POST, not its start, so a POST that ran longer than the
 * interval still leaves a full interval before the retry. */
void espos_ble_post_done(espos_ble_post_sched_t *s, int64_t now_us, bool ok, uint32_t interval_ms);

/* Parsed `gatt_subscribe` sub-descriptors. */
#define ESPOS_BLE_UUID_MAX       40
#define ESPOS_BLE_WRITE_DATA_MAX 64

typedef struct {
    char char_uuid[ESPOS_BLE_UUID_MAX];
    uint8_t data[ESPOS_BLE_WRITE_DATA_MAX];
    size_t data_len;
    espos_ble_write_mode_t mode;
} espos_ble_init_write_t;

typedef struct {
    char char_uuid[ESPOS_BLE_UUID_MAX];
    uint32_t interval_ms;
    /* Optional command written before each read; empty when unused. */
    uint8_t write_before_read[ESPOS_BLE_WRITE_DATA_MAX];
    size_t write_before_read_len;
} espos_ble_poll_t;

typedef struct {
    char char_uuid[ESPOS_BLE_UUID_MAX];
    uint8_t data[ESPOS_BLE_WRITE_DATA_MAX];
    size_t data_len;
    uint32_t interval_ms;
    espos_ble_write_mode_t mode;
} espos_ble_periodic_write_t;

/* UUID text <-> 128-bit big-endian bytes.
 *
 * Kept here, away from the Bluedroid headers, because the endianness is the
 * classic place to get this wrong: BLE stores 128-bit UUIDs little-endian in
 * the wire struct, while the text form is big-endian. Testing it on the host
 * beats discovering a reversed UUID as "characteristic not found" on a boat.
 *
 * Accepts the 4-char ("180f"), 8-char ("0000180f") and 36-char dashed forms.
 * `out` receives 16 bytes big-endian and `out_bits` the original width
 * (16/32/128). Returns false on a malformed string. */
bool espos_ble_uuid_parse(const char *str, uint8_t out[16], uint8_t *out_bits);

/* Format 16 big-endian bytes back to text: the short forms when the UUID is
 * in the Bluetooth base range, otherwise the dashed 36-char form. `out` needs
 * ESPOS_BLE_UUID_MAX bytes. Always lowercase - characteristic UUIDs are keyed
 * lowercase throughout. */
void espos_ble_uuid_format(const uint8_t uuid[16], uint8_t bits, char *out);

/* True when two UUIDs denote the same attribute, comparing 128-bit forms so
 * "180f" and its expanded base-range equivalent match. */
bool espos_ble_uuid_equal(const uint8_t a[16], const uint8_t b[16]);

/* Read the optional `with_response` field of a gatt command object.
 * Absent, or not a boolean, means write-with-response - matching the
 * descriptor's default on the server side and the behaviour of every gateway
 * build that predates the flag. `obj_json` is a JSON object. */
espos_ble_write_mode_t espos_ble_parse_write_mode(const void *cjson_obj);

#ifdef __cplusplus
}
#endif
