/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espos_sk_delta.h"

typedef struct {
    char path[ESPOS_SK_PATH_MAX];
    char value[ESPOS_SK_VALUE_MAX];
    uint8_t ctx; /* index into espos_sk_delta.ctx, in order of first publish */
} pending_t;

#define SELF_CONTEXT "vessels.self"

/* A queued message and the monotonic instant its values were batched. The
 * stamp is kept beside the text rather than written into it because the wall
 * clock may not exist yet when the message is built — the whole point of
 * timestamping a buffered delta is that the device can learn the time later
 * and still date the data correctly. */
typedef struct {
    char *msg;
    uint32_t batch_ms; /* monotonic ms when this batch closed */
    bool stamped;      /* the text already carries its timestamp: do not add another */
} queued_t;

struct espos_sk_delta {
    espos_sk_delta_cfg_t cfg;
    char label[40];
    pending_t pending[ESPOS_SK_PENDING_MAX];
    size_t pending_n;
    /* The contexts the open window holds values for. A delta carries one
     * context, so each becomes its own message when the window closes; a
     * small table rather than a context per pending value, because a device
     * publishing for another vessel publishes for one or two, and the
     * difference is 2.5 KB of RAM on a classic ESP32. */
    char ctx[ESPOS_SK_CONTEXTS_MAX][ESPOS_SK_CONTEXT_MAX];
    size_t ctx_n;
    uint32_t pending_since_ms;
    queued_t *ring;           /* messages, oldest at head */
    size_t head, count;
    size_t bytes;
    uint32_t last_take_ms;
    bool draining;            /* backlog existed when we started sending */
    int64_t (*wall_ms)(void *arg); /* wall clock; NULL or returning 0 = no timestamps */
    void *wall_arg;
    espos_sk_delta_stats_t st;
};

espos_sk_delta_t *espos_sk_delta_create(const espos_sk_delta_cfg_t *cfg)
{
    espos_sk_delta_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->cfg = *cfg;
    if (d->cfg.batch_ms == 0) {
        d->cfg.batch_ms = 100;
    }
    if (d->cfg.max_msgs == 0) {
        d->cfg.max_msgs = 1;
    }
    if (d->cfg.drain_per_s == 0) {
        d->cfg.drain_per_s = 20;
    }
    d->ring = calloc(d->cfg.max_msgs, sizeof(*d->ring));
    if (!d->ring) {
        free(d);
        return NULL;
    }
    snprintf(d->label, sizeof(d->label), "%s", cfg->label ? cfg->label : "espos");
    return d;
}

void espos_sk_delta_destroy(espos_sk_delta_t *d)
{
    if (!d) {
        return;
    }
    for (size_t i = 0; i < d->count; i++) {
        free(d->ring[(d->head + i) % d->cfg.max_msgs].msg);
    }
    free(d->ring);
    free(d);
}

void espos_sk_delta_set_clock(espos_sk_delta_t *d, int64_t (*wall_ms)(void *arg), void *arg)
{
    if (!d) {
        return;
    }
    d->wall_ms = wall_ms;
    d->wall_arg = arg;
}

void espos_sk_delta_set_label(espos_sk_delta_t *d, const char *label)
{
    snprintf(d->label, sizeof(d->label), "%s", label ? label : "espos");
}

void espos_sk_delta_set_timing(espos_sk_delta_t *d, uint32_t batch_ms, uint32_t drain_per_s)
{
    if (batch_ms) {
        d->cfg.batch_ms = batch_ms;
    }
    if (drain_per_s) {
        d->cfg.drain_per_s = drain_per_s;
    }
}

/* ------------------------------------------------------------ ring */

static void ring_push(espos_sk_delta_t *d, char *msg, uint32_t batch_ms)
{
    size_t len = strlen(msg);
    /* make room: drop oldest while over either limit */
    while (d->count > 0 && (d->count >= d->cfg.max_msgs || (d->cfg.max_bytes && d->bytes + len > d->cfg.max_bytes))) {
        char *old = d->ring[d->head].msg;
        d->bytes -= strlen(old);
        free(old);
        d->head = (d->head + 1) % d->cfg.max_msgs;
        d->count--;
        d->st.dropped++;
    }
    if (d->cfg.max_bytes && len > d->cfg.max_bytes) {
        free(msg); /* a single message larger than the whole buffer */
        d->st.dropped++;
        return;
    }
    size_t slot = (d->head + d->count) % d->cfg.max_msgs;
    d->ring[slot].msg = msg;
    d->ring[slot].batch_ms = batch_ms;
    d->ring[slot].stamped = false;
    d->count++;
    d->bytes += len;
}

static char *ring_pop(espos_sk_delta_t *d, uint32_t *batch_ms, bool *stamped)
{
    if (d->count == 0) {
        return NULL;
    }
    char *m = d->ring[d->head].msg;
    *batch_ms = d->ring[d->head].batch_ms;
    *stamped = d->ring[d->head].stamped;
    d->ring[d->head].msg = NULL;
    d->head = (d->head + 1) % d->cfg.max_msgs;
    d->count--;
    d->bytes -= strlen(m);
    return m;
}

/* A requeued message has already had its timestamp written in, so it goes back
 * as it is: its batch_ms is irrelevant from here on, and re-stamping it on the
 * next take would only move the time of a measurement that did not move. */
void espos_sk_delta_requeue(espos_sk_delta_t *d, char *msg)
{
    if (!msg) {
        return;
    }
    if (d->count >= d->cfg.max_msgs) {
        /* full: this one is the oldest, drop it instead of a newer one */
        free(msg);
        d->st.dropped++;
        return;
    }
    d->head = (d->head + d->cfg.max_msgs - 1) % d->cfg.max_msgs;
    d->ring[d->head].msg = msg;
    d->ring[d->head].batch_ms = 0;
    d->ring[d->head].stamped = true;
    d->count++;
    d->bytes += strlen(msg);
    d->st.taken--;
}

/* --------------------------------------------------------- pending */

/* The context is copied into the message as it is, so it must not be able to
 * end the JSON string it lands in. */
static esp_err_t context_check(const char *c)
{
    if (!*c) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(c) >= ESPOS_SK_CONTEXT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == '"' || *c == '\\') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

/* Index of `context` in the window's table, adding it when there is room;
 * -1 when the table is full. */
static int context_slot(espos_sk_delta_t *d, const char *context)
{
    for (size_t i = 0; i < d->ctx_n; i++) {
        if (strcmp(d->ctx[i], context) == 0) {
            return (int)i;
        }
    }
    if (d->ctx_n == ESPOS_SK_CONTEXTS_MAX) {
        return -1;
    }
    strcpy(d->ctx[d->ctx_n], context);
    return (int)d->ctx_n++;
}

esp_err_t espos_sk_delta_publish(espos_sk_delta_t *d, const char *path, const char *value_json, uint32_t now_ms)
{
    return espos_sk_delta_publish_ctx(d, NULL, path, value_json, now_ms);
}

esp_err_t espos_sk_delta_publish_ctx(espos_sk_delta_t *d, const char *context, const char *path, const char *value_json,
                                     uint32_t now_ms)
{
    if (!d || !path || !*path || !value_json || !*value_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context) {
        context = SELF_CONTEXT;
    }
    esp_err_t err = context_check(context);
    if (err != ESP_OK) {
        return err;
    }
    if (strlen(path) >= ESPOS_SK_PATH_MAX || strlen(value_json) >= ESPOS_SK_VALUE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    int ctx = context_slot(d, context);
    if (ctx >= 0) {
        for (size_t i = 0; i < d->pending_n; i++) {
            if (d->pending[i].ctx == ctx && strcmp(d->pending[i].path, path) == 0) {
                strcpy(d->pending[i].value, value_json); /* newest wins within the window */
                return ESP_OK;
            }
        }
    }
    if (ctx < 0 || d->pending_n == ESPOS_SK_PENDING_MAX) {
        espos_sk_delta_flush(d, now_ms); /* window overflow: close it early */
        ctx = context_slot(d, context);
    }
    if (d->pending_n == 0) {
        d->pending_since_ms = now_ms;
    }
    strcpy(d->pending[d->pending_n].path, path);
    strcpy(d->pending[d->pending_n].value, value_json);
    d->pending[d->pending_n].ctx = (uint8_t)ctx;
    d->pending_n++;
    d->st.pending = d->pending_n;
    return ESP_OK;
}

static char *build_message(espos_sk_delta_t *d, uint8_t ctx)
{
    size_t need = 96 + strlen(d->label) + strlen(d->ctx[ctx]);
    for (size_t i = 0; i < d->pending_n; i++) {
        if (d->pending[i].ctx == ctx) {
            need += 24 + strlen(d->pending[i].path) + strlen(d->pending[i].value);
        }
    }
    char *m = malloc(need);
    if (!m) {
        return NULL;
    }
    int n = snprintf(m, need, "{\"context\":\"%s\",\"updates\":[{\"source\":{\"label\":\"%s\"},\"values\":[", d->ctx[ctx],
                     d->label);
    bool first = true;
    for (size_t i = 0; i < d->pending_n; i++) {
        if (d->pending[i].ctx != ctx) {
            continue;
        }
        n += snprintf(m + n, need - n, "%s{\"path\":\"%s\",\"value\":%s}", first ? "" : ",", d->pending[i].path,
                      d->pending[i].value);
        first = false;
    }
    snprintf(m + n, need - n, "]}]}");
    return m;
}

void espos_sk_delta_flush(espos_sk_delta_t *d, uint32_t now_ms)
{
    if (!d || d->pending_n == 0) {
        return;
    }
    /* The values in this batch were published between pending_since_ms and
     * now; the batch window is at most a few hundred milliseconds wide, so the
     * instant it closed dates all of them well enough. Recording it here — not
     * at take() — is what lets a message buffered through an outage keep the
     * time it was measured rather than the time it was finally sent. */
    uint32_t batch_ms = d->pending_since_ms;
    (void)now_ms;
    for (size_t c = 0; c < d->ctx_n; c++) {
        bool used = false;
        for (size_t i = 0; i < d->pending_n && !used; i++) {
            used = d->pending[i].ctx == c;
        }
        if (!used) {
            continue;
        }
        char *m = build_message(d, (uint8_t)c);
        if (m) {
            d->st.built++;
            ring_push(d, m, batch_ms);
        }
    }
    d->pending_n = 0;
    d->ctx_n = 0;
    d->st.pending = 0;
}

static bool batch_due(const espos_sk_delta_t *d, uint32_t now_ms)
{
    return d->pending_n > 0 && (int32_t)(now_ms - (d->pending_since_ms + d->cfg.batch_ms)) >= 0;
}

/* --------------------------------------------------------- timestamps */

/* Where build_message() puts the marker we splice in front of. The update
 * object opens with its source, so the timestamp goes immediately before it —
 * a SignalK `updates[]` member takes its members in any order. */
#define UPDATE_HEAD "\"updates\":[{"

/* ISO 8601 UTC with milliseconds: "2026-09-07T10:12:13.456Z". Written here
 * rather than borrowed from espos_time because this engine is pure C with no
 * component dependencies — it is host-tested on its own, and a delta batcher
 * that could not be built without a clock component would be the wrong shape.
 * Civil-calendar arithmetic, no libc, no timezone. */
static size_t iso8601(int64_t unix_ms, char *buf, size_t size)
{
    if (unix_ms <= 0 || size < 25) {
        return 0;
    }
    int64_t secs = unix_ms / 1000;
    int32_t ms = (int32_t)(unix_ms % 1000);
    int64_t days = secs / 86400;
    int32_t sod = (int32_t)(secs % 86400);
    /* Howard Hinnant's civil_from_days: March-based era arithmetic, which
     * makes the leap day the last day of the year and removes every special
     * case from the month lengths. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned mon = mp + (mp < 10 ? 3 : -9);
    y += (mon <= 2);
    int n = snprintf(buf, size, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", (long long)y, mon, day, (int)(sod / 3600),
                     (int)((sod / 60) % 60), (int)(sod % 60), (int)ms);
    return (n > 0 && (size_t)n < size) ? (size_t)n : 0;
}

/**
 * Return `msg` with `"timestamp":"…",` inserted at the head of its update
 * object, or `msg` untouched when there is no clock, no room, or the message
 * does not have the shape build_message() produces (a requeued or otherwise
 * foreign string). Takes ownership either way and returns what the caller
 * should send: on success the original is freed and a new buffer returned.
 *
 * The instant is `wall_now - (mono_now - batch_ms)`: how long ago the batch
 * closed, subtracted from what the clock says now. Both terms are read at the
 * same moment, so a clock set long after the message was buffered still dates
 * it correctly — which is the entire reason this exists.
 */
static char *stamp(espos_sk_delta_t *d, char *msg, uint32_t now_ms, uint32_t batch_ms)
{
    if (!d->wall_ms) {
        return msg;
    }
    int64_t wall_now = d->wall_ms(d->wall_arg);
    if (wall_now <= 0) {
        return msg; /* the device does not know the time: the server stamps it */
    }
    /* Unsigned difference on purpose: the monotonic counter wraps at ~49 days
     * and the subtraction stays correct across the wrap. */
    uint32_t age_ms = now_ms - batch_ms;
    char iso[25];
    if (iso8601(wall_now - (int64_t)age_ms, iso, sizeof(iso)) == 0) {
        return msg;
    }
    char *at = strstr(msg, UPDATE_HEAD);
    if (!at) {
        return msg; /* not a message this engine built */
    }
    size_t head = (size_t)(at - msg) + strlen(UPDATE_HEAD);
    size_t tail = strlen(msg) - head;
    size_t add = strlen("\"timestamp\":\"") + strlen(iso) + strlen("\",");
    char *out = malloc(head + add + tail + 1);
    if (!out) {
        return msg; /* out of memory: send it unstamped rather than not at all */
    }
    memcpy(out, msg, head);
    /* `add` is the exact length of what is written, so snprintf never
     * truncates here; the check is what keeps that an assertion rather than an
     * assumption, because a wrong `add` would put the tail in the wrong place. */
    int n = snprintf(out + head, add + 1, "\"timestamp\":\"%s\",", iso);
    if (n < 0 || (size_t)n != add) {
        free(out);
        return msg;
    }
    memcpy(out + head + add, msg + head, tail + 1);
    free(msg);
    return out;
}

char *espos_sk_delta_take(espos_sk_delta_t *d, uint32_t now_ms, bool connected)
{
    if (!d) {
        return NULL;
    }
    if (batch_due(d, now_ms)) {
        espos_sk_delta_flush(d, now_ms);
    }
    if (!connected || d->count == 0) {
        d->draining = false;
        return NULL;
    }
    /* Backlog present (more than the message we just built): rate-limit. */
    if (d->count > 1 || d->draining) {
        uint32_t gap = 1000 / d->cfg.drain_per_s;
        if (d->st.taken && (int32_t)(now_ms - (d->last_take_ms + gap)) < 0) {
            return NULL;
        }
        d->draining = d->count > 1;
    }
    uint32_t batch_ms = now_ms;
    bool stamped = false;
    char *m = ring_pop(d, &batch_ms, &stamped);
    d->last_take_ms = now_ms;
    d->st.taken++;
    d->st.buffered = d->count;
    d->st.buffered_bytes = d->bytes;
    if (m && !stamped) {
        m = stamp(d, m, now_ms, batch_ms);
    }
    return m;
}

uint32_t espos_sk_delta_next_due_ms(const espos_sk_delta_t *d, uint32_t now_ms, bool connected)
{
    uint32_t due = UINT32_MAX;
    if (d->pending_n > 0) {
        int32_t left = (int32_t)((d->pending_since_ms + d->cfg.batch_ms) - now_ms);
        due = left > 0 ? (uint32_t)left : 0;
    }
    if (connected && d->count > 0) {
        uint32_t gap = 1000 / d->cfg.drain_per_s;
        int32_t left = (int32_t)((d->last_take_ms + gap) - now_ms);
        uint32_t w = (d->count > 1 || d->draining) && d->st.taken && left > 0 ? (uint32_t)left : 0;
        if (w < due) {
            due = w;
        }
    }
    return due;
}

void espos_sk_delta_stats(const espos_sk_delta_t *d, espos_sk_delta_stats_t *out)
{
    *out = d->st;
    out->pending = d->pending_n;
    out->buffered = d->count;
    out->buffered_bytes = d->bytes;
}

/* --------------------------------------------------------- helpers */

int espos_sk_json_number(char *buf, size_t size, double v)
{
    if (isnan(v) || isinf(v)) {
        return snprintf(buf, size, "null");
    }
    /* Values that are exactly a float (sensor floats promoted to double)
     * get the shortest float representation ("0.1", not 0.100000001490116);
     * genuine doubles get up to 15 significant digits. */
    bool is_float = (double)(float)v == v;
    for (int prec = 6; prec <= 15; prec++) {
        int n = snprintf(buf, size, "%.*g", prec, v);
        if (is_float ? (strtof(buf, NULL) == (float)v) : (strtod(buf, NULL) == v)) {
            return n;
        }
        if (prec == 15) {
            return n;
        }
    }
    return 0;
}

int espos_sk_json_string(char *buf, size_t size, const char *s)
{
    size_t o = 0;
    if (size < 3) {
        return 0;
    }
    buf[o++] = '"';
    for (; *s && o + 7 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[o++] = '\\';
            buf[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(buf + o, size - o, "\\u%04x", c);
        } else {
            buf[o++] = (char)c;
        }
    }
    buf[o++] = '"';
    buf[o] = '\0';
    return (int)o;
}
