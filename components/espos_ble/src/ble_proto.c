/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wire-format helpers - see ble_proto.h. Pure C, host-testable.
 */

#include "ble_proto.h"

#include <string.h>

#include "cJSON.h"

static const char kHexUpper[] = "0123456789ABCDEF";
static const char kHexLower[] = "0123456789abcdef";

static void hex_encode(const uint8_t *data, size_t len, char *out, const char *digits)
{
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/* The case difference between these two is not cosmetic: the advertisement
 * POST body uses uppercase and the GATT control frames use lowercase. Keep
 * them apart so a future edit cannot quietly unify them. */
void espos_ble_hex_encode_upper(const uint8_t *data, size_t len, char *out)
{
    hex_encode(data, len, out, kHexUpper);
}

void espos_ble_hex_encode_lower(const uint8_t *data, size_t len, char *out)
{
    hex_encode(data, len, out, kHexLower);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int espos_ble_hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    if (!hex || !out) return -1;
    size_t len = strlen(hex);
    if (len % 2 != 0) return -1; /* odd length is malformed, not truncatable */
    size_t bytes = len / 2;
    if (bytes > out_max) return -1;

    for (size_t i = 0; i < bytes; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)bytes;
}

void espos_ble_advq_init(espos_ble_advq_t *q, espos_ble_adv_t *storage, size_t cap)
{
    q->slots = storage;
    q->cap = cap;
    q->head = 0;
    q->count = 0;
    q->dropped = 0;
}

bool espos_ble_advq_push(espos_ble_advq_t *q, const espos_ble_adv_t *adv)
{
    if (!q || !q->cap) return false;

    bool evicted = (q->count == q->cap);
    q->slots[q->head] = *adv;
    q->head = (q->head + 1) % q->cap;
    if (evicted) {
        /* Full: head has just overwritten the oldest entry, so the count is
         * unchanged and one advertisement is gone. Counting here (rather than
         * from a size delta after the fact) is what keeps the tally honest. */
        q->dropped++;
    } else {
        q->count++;
    }
    return evicted;
}

bool espos_ble_post_due(const espos_ble_post_sched_t *s, int64_t now_us, size_t pending, size_t batch_max,
                        size_t capacity)
{
    if (now_us >= s->next_us) return true;
    size_t full = batch_max < capacity ? batch_max : capacity;
    return !s->held && full && pending >= full;
}

void espos_ble_post_done(espos_ble_post_sched_t *s, int64_t now_us, bool ok, uint32_t interval_ms)
{
    s->held = !ok;
    s->next_us = now_us + (int64_t)interval_ms * 1000;
}

size_t espos_ble_advq_drain(espos_ble_advq_t *q, espos_ble_adv_t *out, size_t max)
{
    if (!q || !out) return 0;
    size_t n = q->count < max ? q->count : max;
    /* Oldest entry sits `count` slots behind head, modulo capacity. */
    size_t tail = (q->head + q->cap - q->count) % q->cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = q->slots[(tail + i) % q->cap];
    }
    q->count -= n;
    return n;
}

espos_ble_write_mode_t espos_ble_parse_write_mode(const void *cjson_obj)
{
    const cJSON *obj = (const cJSON *)cjson_obj;
    const cJSON *wr = cJSON_GetObjectItemCaseSensitive(obj, "with_response");
    /* Absent or non-boolean -> with-response. Anything else would silently
     * change the behaviour of every existing server command that omits it. */
    if (!cJSON_IsBool(wr)) return ESPOS_BLE_WRITE_WITH_RESPONSE;
    return cJSON_IsTrue(wr) ? ESPOS_BLE_WRITE_WITH_RESPONSE
                            : ESPOS_BLE_WRITE_NO_RESPONSE;
}

/* The Bluetooth base UUID: 0000xxxx-0000-1000-8000-00805F9B34FB. A 16- or
 * 32-bit UUID is shorthand for this with the short value in bytes 0-3. */
static const uint8_t kBaseUuid[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
};

static int hex_byte(const char *s)
{
    int hi = hex_val(s[0]);
    int lo = hex_val(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

bool espos_ble_uuid_parse(const char *str, uint8_t out[16], uint8_t *out_bits)
{
    if (!str || !out) return false;
    size_t len = strlen(str);

    /* Short forms expand into the base UUID, so everything downstream can
     * compare a single 128-bit representation. */
    if (len == 4 || len == 8) {
        memcpy(out, kBaseUuid, 16);
        size_t nbytes = len / 2;
        for (size_t i = 0; i < nbytes; i++) {
            int b = hex_byte(str + i * 2);
            if (b < 0) return false;
            /* Right-align into bytes 0..3 (big-endian). */
            out[(4 - nbytes) + i] = (uint8_t)b;
        }
        if (out_bits) *out_bits = (len == 4) ? 16 : 32;
        return true;
    }

    if (len == 36) {
        static const int dash[] = { 8, 13, 18, 23 };
        for (size_t i = 0; i < 4; i++) {
            if (str[dash[i]] != '-') return false;
        }
        size_t o = 0;
        for (size_t i = 0; i < 36 && o < 16; i++) {
            if (str[i] == '-') continue;
            int b = hex_byte(str + i);
            if (b < 0) return false;
            out[o++] = (uint8_t)b;
            i++; /* hex_byte consumed two characters */
        }
        if (o != 16) return false;
        if (out_bits) *out_bits = 128;
        return true;
    }

    return false;
}

void espos_ble_uuid_format(const uint8_t uuid[16], uint8_t bits, char *out)
{
    /* Only shorten when the UUID really is in the base range - otherwise a
     * vendor UUID that happens to be 16-bit-ish would be mangled. */
    bool base = (memcmp(uuid + 4, kBaseUuid + 4, 12) == 0);

    if (base && bits == 16 && uuid[0] == 0 && uuid[1] == 0) {
        snprintf(out, ESPOS_BLE_UUID_MAX, "%02x%02x", uuid[2], uuid[3]);
        return;
    }
    if (base && bits == 32) {
        snprintf(out, ESPOS_BLE_UUID_MAX, "%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3]);
        return;
    }
    snprintf(out, ESPOS_BLE_UUID_MAX,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
             uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12],
             uuid[13], uuid[14], uuid[15]);
}

bool espos_ble_uuid_equal(const uint8_t a[16], const uint8_t b[16])
{
    return memcmp(a, b, 16) == 0;
}
