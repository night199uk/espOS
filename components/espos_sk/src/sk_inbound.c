/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inbound side of the stream: subscription table, dispatch of parsed
 * updates, PUT request tracking, raw outbound frames. Everything the
 * stream task needs is behind espos_sk_inbound_* (sk_ws.c calls those);
 * the public espos_sk_subscribe/put/send_raw are thread-safe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"

#include "espos_sk.h"
#include "espos_sk_delta.h"
#include "espos_sk_priv.h"

static const char *TAG = "espos_skin";

#define MAX_PUTS       8
#define PUT_TIMEOUT_MS 10000
#define MAX_RAW        16

typedef struct {
    int handle;
    char pattern[ESPOS_SK_PATH_MAX];
    uint32_t period_ms;
    espos_sk_sub_cb_t cb;
    void *arg;
    bool sent;                 /* included in a subscribe frame on the live connection */
} sub_t;

typedef struct {
    char id[40];
    espos_sk_put_cb_t cb;
    void *arg;
    uint32_t sent_ms;
    bool used;
} put_t;

/* Inbound: a path this device lets the server operate. Exact paths only --
 * a wildcard handler would have to decide what a PUT to "electrical.*"
 * means, and every honest answer to that is a per-path handler anyway. */
typedef struct {
    char path[ESPOS_SK_PATH_MAX];
    espos_sk_put_handler_t cb;
    void *arg;
    bool used;
} put_handler_t;

/* Tables are heap-allocated on first use: ~8 KiB of static .bss here was
 * enough to push the ESP32-P4 main task stack out of internal RAM into
 * SPM (esp_hosted eats most of RETENT_RAM before app_main), where the
 * first flash op asserts. Keep .bss small. */
static struct {
    SemaphoreHandle_t lock;
    sub_t *subs;
    size_t n_subs;
    int next_handle;
    bool dirty;                /* subs changed: (re)send subscribe frame */
    char pending_unsub[4][ESPOS_SK_PATH_MAX];
    size_t n_unsub;
    put_t puts[MAX_PUTS];
    put_handler_t *handlers;   /* ESPOS_SK_MAX_PUT_HANDLERS, allocated on first register */
    uint32_t puts_in, puts_rejected;
    char *raw[MAX_RAW];        /* outbound frames waiting for the stream task */
    size_t raw_head, raw_n;
    bool connected;
    uint32_t received, frames, puts_sent, puts_failed;
} s;

static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

static void ensure_init(void)
{
    if (!s.lock) {
        s.subs = calloc(ESPOS_SK_SUBS_CAP, sizeof(sub_t));
        s.lock = xSemaphoreCreateMutex();
        s.next_handle = 1;
    }
}

/* ---------------------------------------------------------- public */

int espos_sk_subscribe(const char *pattern, uint32_t period_ms, espos_sk_sub_cb_t cb, void *arg)
{
    if (!pattern || !*pattern || !cb || strlen(pattern) >= ESPOS_SK_PATH_MAX) {
        return -ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    lock();
    if (!s.subs || s.n_subs >= ESPOS_SK_SUBS_CAP) {
        unlock();
        return -ESP_ERR_NO_MEM;
    }
    sub_t *e = &s.subs[s.n_subs++];
    memset(e, 0, sizeof(*e));
    e->handle = s.next_handle++;
    snprintf(e->pattern, sizeof(e->pattern), "%s", pattern);
    e->period_ms = period_ms ? period_ms : 1000;
    e->cb = cb;
    e->arg = arg;
    s.dirty = true;
    int h = e->handle;
    unlock();
    return h;
}

esp_err_t espos_sk_unsubscribe(int handle)
{
    ensure_init();
    lock();
    for (size_t i = 0; i < s.n_subs; i++) {
        if (s.subs[i].handle == handle) {
            /* only tell the server if no other sub covers the same pattern */
            bool other = false;
            for (size_t j = 0; j < s.n_subs; j++) {
                if (j != i && strcmp(s.subs[j].pattern, s.subs[i].pattern) == 0) {
                    other = true;
                }
            }
            if (!other && s.subs[i].sent && s.n_unsub < 4) {
                snprintf(s.pending_unsub[s.n_unsub++], ESPOS_SK_PATH_MAX, "%s", s.subs[i].pattern);
                s.dirty = true;
            }
            memmove(&s.subs[i], &s.subs[i + 1], (s.n_subs - i - 1) * sizeof(sub_t));
            s.n_subs--;
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

static void uuid4(char out[40])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static esp_err_t enqueue_raw_locked(char *frame)
{
    if (s.raw_n >= MAX_RAW) {
        return ESP_ERR_NO_MEM;
    }
    s.raw[(s.raw_head + s.raw_n) % MAX_RAW] = frame;
    s.raw_n++;
    return ESP_OK;
}

esp_err_t espos_sk_send_raw(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    lock();
    if (!s.connected) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(json);
    esp_err_t err = copy ? enqueue_raw_locked(copy) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        free(copy);
    }
    unlock();
    return err;
}

esp_err_t espos_sk_put(const char *path, const char *value_json, espos_sk_put_cb_t cb, void *arg)
{
    if (!path || !*path || !value_json || strlen(path) >= ESPOS_SK_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    lock();
    if (!s.connected) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    put_t *slot = NULL;
    for (size_t i = 0; i < MAX_PUTS; i++) {
        if (!s.puts[i].used) {
            slot = &s.puts[i];
            break;
        }
    }
    if (!slot) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    char id[40];
    uuid4(id);
    size_t n = strlen(path) + strlen(value_json) + 160;   /* envelope + 36-char uuid */
    char *frame = malloc(n);
    if (!frame) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    snprintf(frame, n, "{\"context\":\"vessels.self\",\"requestId\":\"%s\",\"put\":{\"path\":\"%s\",\"value\":%s}}", id, path, value_json);
    esp_err_t err = enqueue_raw_locked(frame);
    if (err != ESP_OK) {
        free(frame);
        unlock();
        return err;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    strcpy(slot->id, id);
    slot->cb = cb;
    slot->arg = arg;
    slot->sent_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    unlock();
    return ESP_OK;
}

/* ------------------------------------------------- inbound PUT handlers */

esp_err_t espos_sk_put_handler_register(const char *path, espos_sk_put_handler_t cb, void *arg)
{
    if (!path || !*path || !cb || strlen(path) >= ESPOS_SK_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    lock();
    if (!s.handlers) {
        s.handlers = calloc(ESPOS_SK_MAX_PUT_HANDLERS, sizeof(put_handler_t));
        if (!s.handlers) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    put_handler_t *free_slot = NULL;
    for (size_t i = 0; i < ESPOS_SK_MAX_PUT_HANDLERS; i++) {
        if (s.handlers[i].used && strcmp(s.handlers[i].path, path) == 0) {
            /* Re-registering a path replaces it rather than adding a second
             * handler: two handlers for one path have no defined winner, and
             * the common cause of a repeat is a node being rebuilt. */
            s.handlers[i].cb = cb;
            s.handlers[i].arg = arg;
            unlock();
            return ESP_OK;
        }
        if (!s.handlers[i].used && !free_slot) {
            free_slot = &s.handlers[i];
        }
    }
    if (!free_slot) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->path, sizeof(free_slot->path), "%s", path);
    free_slot->cb = cb;
    free_slot->arg = arg;
    free_slot->used = true;
    unlock();
    ESP_LOGI(TAG, "PUT handler for %s", path);
    return ESP_OK;
}

esp_err_t espos_sk_put_handler_unregister(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    lock();
    for (size_t i = 0; s.handlers && i < ESPOS_SK_MAX_PUT_HANDLERS; i++) {
        if (s.handlers[i].used && strcmp(s.handlers[i].path, path) == 0) {
            s.handlers[i].used = false;
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espos_sk_put_respond(const char *request_id, const char *state, int status_code, const char *message)
{
    if (!request_id || !*request_id || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    /* Built by the parser half (pure, host-tested: the exact shape
     * signalk-server accepts) and queued like any other frame, so it goes
     * out behind whatever the stream task is already sending. */
    char *frame = espos_sk_put_response_frame(request_id, state, status_code, message);
    if (!frame) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    esp_err_t err = s.connected ? enqueue_raw_locked(frame) : ESP_ERR_INVALID_STATE;
    unlock();
    if (err != ESP_OK) {
        free(frame);
    }
    return err;
}

/* ------------------------------------------- called by the stream task */

void espos_sk_inbound_set_connected(bool connected)
{
    ensure_init();
    lock();
    s.connected = connected;
    if (!connected) {
        for (size_t i = 0; i < s.n_subs; i++) {
            s.subs[i].sent = false;
        }
        s.n_unsub = 0;
        s.dirty = s.n_subs > 0;
        /* pending PUTs cannot complete any more */
        for (size_t i = 0; i < MAX_PUTS; i++) {
            if (s.puts[i].used) {
                put_t p = s.puts[i];
                s.puts[i].used = false;
                s.puts_failed++;
                unlock();
                if (p.cb) {
                    p.cb(p.id, "FAILED", 0, "stream closed", p.arg);
                }
                lock();
            }
        }
        for (size_t i = 0; i < s.raw_n; i++) {
            free(s.raw[(s.raw_head + i) % MAX_RAW]);
        }
        s.raw_n = 0;
    }
    unlock();
}

/* Build the subscribe (and unsubscribe) frames for what changed. Returns
 * malloc'ed frame or NULL. Call repeatedly until NULL. */
char *espos_sk_inbound_take_frame(void)
{
    ensure_init();
    lock();
    char *out = NULL;
    if (s.raw_n) {
        out = s.raw[s.raw_head];
        s.raw_head = (s.raw_head + 1) % MAX_RAW;
        s.raw_n--;
        unlock();
        return out;
    }
    if (s.n_unsub) {
        size_t cap = 64 + s.n_unsub * (ESPOS_SK_PATH_MAX + 16);
        out = malloc(cap);
        if (out) {
            int n = snprintf(out, cap, "{\"context\":\"vessels.self\",\"unsubscribe\":[");
            for (size_t i = 0; i < s.n_unsub; i++) {
                n += snprintf(out + n, cap - n, "%s{\"path\":\"%s\"}", i ? "," : "", s.pending_unsub[i]);
            }
            snprintf(out + n, cap - n, "]}");
        }
        s.n_unsub = 0;
        unlock();
        return out;
    }
    if (s.dirty) {
        s.dirty = false;
        size_t todo = 0;
        for (size_t i = 0; i < s.n_subs; i++) {
            if (!s.subs[i].sent) {
                todo++;
            }
        }
        if (todo) {
            size_t cap = 64 + todo * (ESPOS_SK_PATH_MAX + 64);
            out = malloc(cap);
            if (out) {
                int n = snprintf(out, cap, "{\"context\":\"vessels.self\",\"subscribe\":[");
                bool first = true;
                for (size_t i = 0; i < s.n_subs; i++) {
                    if (s.subs[i].sent) {
                        continue;
                    }
                    /* the same pattern once, whichever period is shortest */
                    bool dup = false;
                    for (size_t j = 0; j < i; j++) {
                        if (!s.subs[j].sent && strcmp(s.subs[j].pattern, s.subs[i].pattern) == 0) {
                            dup = true;
                        }
                    }
                    if (!dup) {
                        n += snprintf(out + n, cap - n, "%s{\"path\":\"%s\",\"period\":%u,\"format\":\"delta\",\"policy\":\"instant\",\"minPeriod\":%u}",
                                      first ? "" : ",", s.subs[i].pattern, (unsigned)s.subs[i].period_ms, (unsigned)s.subs[i].period_ms);
                        first = false;
                    }
                }
                snprintf(out + n, cap - n, "]}");
                for (size_t i = 0; i < s.n_subs; i++) {
                    s.subs[i].sent = true;
                }
            }
        }
    }
    unlock();
    return out;
}

static bool dispatch(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    /* Copy matching subscribers out under the lock, call them without it. */
    struct {
        espos_sk_sub_cb_t cb;
        void *arg;
    } hit[8];
    size_t n = 0;
    lock();
    for (size_t i = 0; i < s.n_subs && n < 8; i++) {
        if (espos_sk_path_matches(s.subs[i].pattern, u->path)) {
            hit[n].cb = s.subs[i].cb;
            hit[n].arg = s.subs[i].arg;
            n++;
        }
    }
    if (n) {
        s.received++;
    }
    unlock();
    for (size_t i = 0; i < n; i++) {
        hit[i].cb(u, hit[i].arg);
    }
    return true;
}

/* One item of an inbound PUT. Runs on the stream task, inside the parse.
 *
 * The answer is sent per item, because a request names one path in practice
 * (signalk-server's handlePut writes exactly one) and a per-item answer is
 * the only one that can carry a per-item status. */
static bool dispatch_put(const espos_sk_update_t *u, void *arg)
{
    bool *answered = arg;
    espos_sk_put_handler_t cb = NULL;
    void *cb_arg = NULL;
    lock();
    s.puts_in++;
    for (size_t i = 0; s.handlers && i < ESPOS_SK_MAX_PUT_HANDLERS; i++) {
        if (s.handlers[i].used && strcmp(s.handlers[i].path, u->path) == 0) {
            cb = s.handlers[i].cb;
            cb_arg = s.handlers[i].arg;
            break;
        }
    }
    unlock();

    if (!cb) {
        /* No handler: 405, the same answer signalk-server gives for a path
         * nothing can act on (src/put.ts). Answering is what matters -- an
         * ignored request leaves the client waiting the full 60 s. */
        lock();
        s.puts_rejected++;
        unlock();
        ESP_LOGW(TAG, "PUT %s: no handler", u->path);
        if (u->request_id) {
            espos_sk_put_respond(u->request_id, "COMPLETED", 405, "PUT not supported for this path");
            *answered = true;
        }
        return true;
    }

    esp_err_t err = cb(u->path, u->value_json ? u->value_json : "null", cb_arg);
    if (!u->request_id) {
        return true;   /* nothing to answer to */
    }
    if (err == ESPOS_SK_PUT_PENDING) {
        /* The handler owns the answer from here; tell the server to keep
         * waiting so it does not resolve the request on the 60 s timeout
         * without ever hearing why. */
        espos_sk_put_respond(u->request_id, "PENDING", 202, NULL);
        *answered = true;
        return true;
    }
    /* Every terminal answer is state COMPLETED with an HTTP-style code:
     * signalk-server drops a reply whose state is anything but COMPLETED,
     * PENDING or null (isWsRequestReply in src/interfaces/ws.ts), so a
     * "FAILED" state would read as no answer at all. */
    if (err == ESP_OK) {
        espos_sk_put_respond(u->request_id, "COMPLETED", 200, NULL);
    } else if (err == ESP_ERR_INVALID_ARG) {
        espos_sk_put_respond(u->request_id, "COMPLETED", 400, "invalid value");
    } else {
        lock();
        s.puts_rejected++;
        unlock();
        espos_sk_put_respond(u->request_id, "COMPLETED", 502, esp_err_to_name(err));
    }
    *answered = true;
    return true;
}

/* Routes one parsed item to the delta dispatcher or the PUT dispatcher. */
static bool route_item(const espos_sk_update_t *u, void *arg)
{
    if (u->request_id) {
        return dispatch_put(u, arg);
    }
    return dispatch(u, NULL);
}

bool espos_sk_inbound_handle_frame(const char *json, size_t len, char *err_out, size_t err_size)
{
    ensure_init();
    espos_sk_frame_t info;
    lock();
    s.frames++;
    unlock();
    /* One callback for both kinds of item-carrying frame. request_id is set
     * only on a PUT item (see espos_sk_parse.h), which is exactly the
     * question "does this item owe an answer?". */
    bool answered = false;
    espos_sk_frame_parse(json, len, &info, route_item, &answered);
    bool is_error = false;
    switch (info.kind) {
    case ESPOS_SK_FRAME_PUT:
        /* A PUT whose items were all unusable (no path, or no value) still
         * gets an answer: silence costs the client a 60 s timeout, and a
         * malformed request is the case where a clear code helps most. */
        if (!answered && info.request_id) {
            espos_sk_put_respond(info.request_id, "COMPLETED", 400, "no usable path/value in the request");
        }
        break;
    case ESPOS_SK_FRAME_RESPONSE: {
        put_t p = { 0 };
        bool found = false;
        lock();
        for (size_t i = 0; i < MAX_PUTS; i++) {
            if (s.puts[i].used && info.request_id && strcmp(s.puts[i].id, info.request_id) == 0) {
                p = s.puts[i];
                if (!info.state || strcmp(info.state, "PENDING") != 0) {
                    s.puts[i].used = false;
                    if (info.status_code >= 400 || (info.state && strcmp(info.state, "FAILED") == 0)) {
                        s.puts_failed++;
                    } else {
                        s.puts_sent++;
                    }
                }
                found = true;
                break;
            }
        }
        unlock();
        if (found && p.cb) {
            p.cb(p.id, info.state ? info.state : "", info.status_code, info.message ? info.message : "", p.arg);
        } else if (!found) {
            ESP_LOGD(TAG, "response for unknown request %s", info.request_id ? info.request_id : "?");
        }
        break;
    }
    case ESPOS_SK_FRAME_ERROR:
        is_error = true;
        if (err_out) {
            snprintf(err_out, err_size, "%s", info.error ? info.error : "server error");
        }
        break;
    default:
        break;
    }
    espos_sk_frame_free(&info);
    return !is_error;
}

void espos_sk_inbound_tick(uint32_t now_ms)
{
    ensure_init();
    lock();
    for (size_t i = 0; i < MAX_PUTS; i++) {
        if (s.puts[i].used && (int32_t)(now_ms - (s.puts[i].sent_ms + PUT_TIMEOUT_MS)) >= 0) {
            put_t p = s.puts[i];
            s.puts[i].used = false;
            s.puts_failed++;
            unlock();
            ESP_LOGW(TAG, "PUT %s timed out", p.id);
            if (p.cb) {
                p.cb(p.id, "TIMEOUT", 0, "no response", p.arg);
            }
            lock();
        }
    }
    unlock();
}

void espos_sk_inbound_stats(espos_sk_ws_status_t *st)
{
    ensure_init();
    lock();
    st->received = s.received;
    st->frames = s.frames;
    st->subs = s.n_subs;
    size_t pend = 0;
    for (size_t i = 0; i < MAX_PUTS; i++) {
        pend += s.puts[i].used;
    }
    st->puts_pending = pend;
    st->puts_sent = s.puts_sent;
    st->puts_failed = s.puts_failed;
    st->puts_in = s.puts_in;
    st->puts_rejected = s.puts_rejected;
    unlock();
}
