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
    int64_t at_ms; /* unix ms the caller measured it at; 0 = dated when the batch closes */
    uint8_t ctx;   /* index into espos_sk_delta.ctx, in order of first publish */
    uint8_t upd;   /* the update it goes out in; updates are numbered in order of first publish */
} pending_t;

#define SELF_CONTEXT "vessels.self"

/* 2000-01-01T00:00:00.000Z. Anything earlier is uptime or unix seconds passed
 * by mistake, which would otherwise be sent dated 1970. */
#define AT_MS_MIN 946684800000LL
/* 9999-12-31T23:59:59.999Z, the last instant iso8601() can write. */
#define AT_MS_MAX 253402300799999LL

/* A queued message and the monotonic instant its values were batched. The
 * stamp is kept beside the text rather than written into it because the wall
 * clock may not exist yet when the message is built — the whole point of
 * timestamping a buffered delta is that the device can learn the time later
 * and still date the data correctly. */
typedef struct {
    char *msg;
    uint32_t batch_ms; /* monotonic ms when this batch closed */
    size_t stamp_at;   /* offset of the update that takes the batch's timestamp; 0 = none */
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
    size_t upd_n; /* updates the open window holds, all contexts together */
    uint32_t pending_since_ms;
    queued_t *ring;           /* messages, oldest at head */
    size_t head, count;
    size_t bytes;
    uint32_t last_take_ms;
    bool draining;            /* backlog existed when we started sending */
    int64_t (*wall_ms)(void *arg); /* wall clock; NULL or returning 0 = no timestamps */
    void *wall_arg;
    /* The last message take() handed out with its undated update still
     * undated (no clock yet), so a requeue keeps it stampable. */
    const char *unstamped;
    size_t unstamped_at;
    uint32_t unstamped_batch_ms;
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

void espos_sk_delta_set_no_drop(espos_sk_delta_t *d, bool no_drop)
{
    if (d) {
        d->cfg.no_drop = no_drop;
    }
}

/* ------------------------------------------------------------ ring */

/* Queue `msg`. `no_drop` refuses a full ring (frees the message and returns
 * false) instead of evicting the oldest, so the caller can retry; a message
 * larger than the whole buffer always fails. */
static bool ring_push(espos_sk_delta_t *d, char *msg, uint32_t batch_ms, size_t stamp_at, bool no_drop)
{
    size_t len = strlen(msg);
    if (no_drop &&
        (d->count >= d->cfg.max_msgs || (d->cfg.max_bytes && d->bytes + len > d->cfg.max_bytes))) {
        free(msg);
        return false; /* full, and dropping is not allowed */
    }
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
        return false;
    }
    size_t slot = (d->head + d->count) % d->cfg.max_msgs;
    d->ring[slot].msg = msg;
    d->ring[slot].batch_ms = batch_ms;
    d->ring[slot].stamp_at = stamp_at;
    d->count++;
    d->bytes += len;
    return true;
}

static char *ring_pop(espos_sk_delta_t *d, uint32_t *batch_ms, size_t *stamp_at)
{
    if (d->count == 0) {
        return NULL;
    }
    char *m = d->ring[d->head].msg;
    *batch_ms = d->ring[d->head].batch_ms;
    *stamp_at = d->ring[d->head].stamp_at;
    d->ring[d->head].msg = NULL;
    d->head = (d->head + 1) % d->cfg.max_msgs;
    d->count--;
    d->bytes -= strlen(m);
    return m;
}

/* A requeued message that had its timestamp written in goes back as it is:
 * re-stamping it on the next take would only move the time of a measurement
 * that did not move. One taken before the clock was known goes back with its
 * offset and batch time, so the next take can still date it. */
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
    bool undated = msg == d->unstamped;
    d->ring[d->head].msg = msg;
    d->ring[d->head].batch_ms = undated ? d->unstamped_batch_ms : 0;
    d->ring[d->head].stamp_at = undated ? d->unstamped_at : 0;
    d->unstamped = NULL;
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
    return espos_sk_delta_publish_at(d, context, path, value_json, 0, now_ms);
}

/* The update a value of context `ctx` dated `at_ms` joins, or -1 when it would
 * open one. */
static int update_of(const espos_sk_delta_t *d, int ctx, int64_t at_ms)
{
    for (size_t i = 0; i < d->pending_n; i++) {
        if (d->pending[i].ctx == ctx && d->pending[i].at_ms == at_ms) {
            return d->pending[i].upd;
        }
    }
    return -1;
}

/* True when a value for `path` would join update `upd` while a later update of
 * the same context already holds `path`: the newer value would reach the
 * server first, and the server keeps the last value of a path it receives. */
static bool overtakes(const espos_sk_delta_t *d, int ctx, int upd, const char *path)
{
    if (upd < 0) {
        return false; /* a new update goes out last */
    }
    for (size_t j = 0; j < d->pending_n; j++) {
        if (d->pending[j].ctx == ctx && d->pending[j].upd > upd && strcmp(d->pending[j].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool flush_window(espos_sk_delta_t *d, uint32_t now_ms);

esp_err_t espos_sk_delta_publish_at(espos_sk_delta_t *d, const char *context, const char *path, const char *value_json,
                                    int64_t at_ms, uint32_t now_ms)
{
    if (!d || !path || !*path || !value_json || !*value_json || (at_ms && (at_ms < AT_MS_MIN || at_ms > AT_MS_MAX))) {
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
    bool reorder = ctx >= 0 && overtakes(d, ctx, update_of(d, ctx, at_ms), path);
    if (ctx >= 0 && !reorder) {
        for (size_t i = 0; i < d->pending_n; i++) {
            if (d->pending[i].ctx == ctx && d->pending[i].at_ms == at_ms && strcmp(d->pending[i].path, path) == 0) {
                strcpy(d->pending[i].value, value_json); /* newest wins within the window */
                return ESP_OK;
            }
        }
    }
    if (ctx < 0 || reorder || d->pending_n == ESPOS_SK_PENDING_MAX) {
        if (!flush_window(d, now_ms)) { /* window overflow: close it early */
            return ESP_ERR_NO_MEM;      /* no_drop: ring full, keep it and retry */
        }
        ctx = context_slot(d, context);
    }
    if (d->pending_n == 0) {
        d->pending_since_ms = now_ms;
    }
    strcpy(d->pending[d->pending_n].path, path);
    strcpy(d->pending[d->pending_n].value, value_json);
    int upd = update_of(d, ctx, at_ms);
    d->pending[d->pending_n].at_ms = at_ms;
    d->pending[d->pending_n].ctx = (uint8_t)ctx;
    d->pending[d->pending_n].upd = (uint8_t)(upd >= 0 ? upd : (int)d->upd_n++);
    d->pending_n++;
    d->st.pending = d->pending_n;
    return ESP_OK;
}

static size_t iso8601(int64_t unix_ms, char *buf, size_t size);

/* The first entry of update `upd`, or -1 when context `ctx` has none in it. */
static int update_first(const espos_sk_delta_t *d, uint8_t ctx, size_t upd)
{
    for (size_t i = 0; i < d->pending_n; i++) {
        if (d->pending[i].ctx == ctx && d->pending[i].upd == upd) {
            return (int)i;
        }
    }
    return -1;
}

/* One message for context `ctx`, with an update per distinct at_ms in the
 * order each was first published: the server applies updates in array order,
 * and publish keeps a path's newer value out of an update sent ahead of its
 * older one, so each path's values land in the order they were published.
 * The values with no time of their own share the update the batch's
 * timestamp is written into at take(); *stamp_at is where it goes, 0 when
 * every value brought its own. */
static char *build_message(espos_sk_delta_t *d, uint8_t ctx, size_t *stamp_at)
{
    *stamp_at = 0;
    /* An update's frame with its timestamp is 77 bytes plus the label; the
     * rest is slack, because an underestimate here is a heap overflow. */
    size_t need = 32 + strlen(d->ctx[ctx]);
    for (size_t i = 0; i < d->pending_n; i++) {
        if (d->pending[i].ctx == ctx) {
            need += 24 + strlen(d->pending[i].path) + strlen(d->pending[i].value);
        }
    }
    for (size_t u = 0; u < d->upd_n; u++) {
        if (update_first(d, ctx, u) >= 0) {
            need += 128 + strlen(d->label);
        }
    }
    char *m = malloc(need);
    if (!m) {
        return NULL;
    }
    int n = snprintf(m, need, "{\"context\":\"%s\",\"updates\":[", d->ctx[ctx]);
    bool first_update = true;
    for (size_t u = 0; u < d->upd_n; u++) {
        int f = update_first(d, ctx, u);
        if (f < 0) {
            continue;
        }
        int64_t at = d->pending[f].at_ms;
        n += snprintf(m + n, need - n, "%s{", first_update ? "" : ",");
        first_update = false;
        char iso[25];
        if (at && iso8601(at, iso, sizeof(iso))) {
            n += snprintf(m + n, need - n, "\"timestamp\":\"%s\",", iso);
        } else {
            *stamp_at = (size_t)n;
        }
        n += snprintf(m + n, need - n, "\"source\":{\"label\":\"%s\"},\"values\":[", d->label);
        bool first = true;
        for (size_t j = (size_t)f; j < d->pending_n; j++) {
            if (d->pending[j].ctx != ctx || d->pending[j].upd != u) {
                continue;
            }
            n += snprintf(m + n, need - n, "%s{\"path\":\"%s\",\"value\":%s}", first ? "" : ",", d->pending[j].path,
                          d->pending[j].value);
            first = false;
        }
        n += snprintf(m + n, need - n, "]}");
    }
    snprintf(m + n, need - n, "]}");
    return m;
}

/* Close the window: build one message per context and queue it. Under no_drop
 * a window that could fit an empty ring but does not fit now is kept (nothing
 * queued, returns false) so the caller retries once the ring drains. A window
 * too large for the ring ever -- unreachable by message count with the
 * supported config, reachable by bytes -- cannot be helped by waiting, so it
 * is queued with the ordinary drop behaviour instead of blocking forever. */
static bool flush_window(espos_sk_delta_t *d, uint32_t now_ms)
{
    if (!d || d->pending_n == 0) {
        return true;
    }
    /* The values in this batch were published between pending_since_ms and
     * now; the batch window is at most a few hundred milliseconds wide, so the
     * instant it closed dates all of them well enough. Recording it here — not
     * at take() — is what lets a message buffered through an outage keep the
     * time it was measured rather than the time it was finally sent. */
    uint32_t batch_ms = d->pending_since_ms;
    (void)now_ms;
    char *msgs[ESPOS_SK_CONTEXTS_MAX] = {};
    size_t stamp_ats[ESPOS_SK_CONTEXTS_MAX] = {};
    size_t nm = 0;
    size_t total = 0;
    for (size_t c = 0; c < d->ctx_n; c++) {
        bool used = false;
        for (size_t i = 0; i < d->pending_n && !used; i++) {
            used = d->pending[i].ctx == c;
        }
        if (!used) {
            continue;
        }
        size_t stamp_at = 0;
        char *m = build_message(d, (uint8_t)c, &stamp_at);
        if (m) {
            msgs[nm] = m;
            stamp_ats[nm] = stamp_at;
            total += strlen(m);
            nm++;
        }
    }
    bool fits_empty = nm <= d->cfg.max_msgs && (d->cfg.max_bytes == 0 || total <= d->cfg.max_bytes);
    bool no_drop = d->cfg.no_drop && fits_empty;
    if (no_drop && (d->count + nm > d->cfg.max_msgs ||
                    (d->cfg.max_bytes && d->bytes + total > d->cfg.max_bytes))) {
        for (size_t i = 0; i < nm; i++) {
            free(msgs[i]);
        }
        return false; /* full: keep the window, the caller retries */
    }
    for (size_t i = 0; i < nm; i++) {
        d->st.built++;
        ring_push(d, msgs[i], batch_ms, stamp_ats[i], no_drop);
    }
    d->pending_n = 0;
    d->ctx_n = 0;
    d->upd_n = 0;
    d->st.pending = 0;
    return true;
}

esp_err_t espos_sk_delta_flush(espos_sk_delta_t *d, uint32_t now_ms)
{
    return flush_window(d, now_ms) ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool batch_due(const espos_sk_delta_t *d, uint32_t now_ms)
{
    return d->pending_n > 0 && (int32_t)(now_ms - (d->pending_since_ms + d->cfg.batch_ms)) >= 0;
}

/* --------------------------------------------------------- timestamps */

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
 * Return `msg` with `"timestamp":"…",` inserted at offset `at`, the head of
 * the update build_message() left undated, or `msg` untouched when there is no
 * clock, no room, or `at` is not the inside of an object's opening brace. A
 * SignalK `updates[]` member takes its members in any order, so the timestamp
 * goes in front of the source. Takes ownership either way and returns what the
 * caller should send: on success the original is freed and a new buffer
 * returned.
 *
 * The instant is `wall_now - (mono_now - batch_ms)`: how long ago the batch
 * closed, subtracted from what the clock says now. Both terms are read at the
 * same moment, so a clock set long after the message was buffered still dates
 * it correctly — which is the entire reason this exists.
 */
static char *stamp(espos_sk_delta_t *d, char *msg, uint32_t now_ms, uint32_t batch_ms, size_t at)
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
    size_t len = strlen(msg);
    if (at == 0 || at >= len || msg[at - 1] != '{') {
        return msg; /* not a message this engine built */
    }
    size_t head = at;
    size_t tail = len - head;
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
    size_t stamp_at = 0;
    char *m = ring_pop(d, &batch_ms, &stamp_at);
    d->last_take_ms = now_ms;
    d->st.taken++;
    d->st.buffered = d->count;
    d->st.buffered_bytes = d->bytes;
    d->unstamped = NULL;
    if (m && stamp_at) {
        char *out = stamp(d, m, now_ms, batch_ms, stamp_at);
        if (out == m) {
            d->unstamped = m;
            d->unstamped_at = stamp_at;
            d->unstamped_batch_ms = batch_ms;
        }
        m = out;
    }
    return m;
}

uint32_t espos_sk_delta_next_due_ms(const espos_sk_delta_t *d, uint32_t now_ms, bool connected)
{
    uint32_t due = UINT32_MAX;
    if (d->pending_n > 0) {
        int32_t left = (int32_t)((d->pending_since_ms + d->cfg.batch_ms) - now_ms);
        due = left > 0 ? (uint32_t)left : 0;
        /* no_drop keeps a window the full ring cannot take yet. Do not report
         * it due on every tick -- retry after a bounded delay, so a retained
         * backlog cannot spin the sender loop (and starve WiFi recovery). */
        if (due == 0 && d->cfg.no_drop &&
            (d->count >= d->cfg.max_msgs || (d->cfg.max_bytes && d->bytes >= d->cfg.max_bytes))) {
            due = d->cfg.batch_ms ? d->cfg.batch_ms : 100;
        }
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
