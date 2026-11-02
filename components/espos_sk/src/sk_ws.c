/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket delta stream: one task keeps ws://<server>/signalk/v1/stream
 * open with the access token, sends batched deltas from the delta engine
 * (draining the offline buffer after a reconnect), reconciles declared
 * metadata on every connect and publishes device telemetry (uptime, heap,
 * RSSI). Built on IDF's esp_transport_ws (no extra dependency); the transport
 * answers pings. Raises skLinkStalled when the stream stays down while WiFi
 * claims to be up — the one network condition a restart fixes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#if CONFIG_ESPOS_SK_TLS
#include "esp_transport_ssl.h"
#endif
#include "esp_transport_ws.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_health.h"
#include "espos_httpd_sse.h"
#include "espos_net.h"
#include "espos_sk.h"
#include "espos_sk_delta.h"
#include "espos_time.h"

#include "espos_sk_priv.h"
#if CONFIG_ESPOS_SK_TLS
#include "espos_sk_tls.h"
#endif

/* tcp_transport builds the whole upgrade request, bearer token of up to
 * ESPOS_SK_TOKEN_MAX included, in this buffer and refuses to dial when it does
 * not fit. espos_core's lint says the same for a full espOS build; this covers
 * a project that takes espos_sk from the registry without it. */
#if !CONFIG_IDF_TARGET_LINUX && defined(CONFIG_WS_BUFFER_SIZE) && CONFIG_WS_BUFFER_SIZE < 2048
#error "espos_sk: CONFIG_WS_BUFFER_SIZE is below 2048, too small for the WebSocket upgrade with a long token. Set CONFIG_WS_BUFFER_SIZE=2048 with idf.py menuconfig, or in sdkconfig.defaults and then delete sdkconfig so it is regenerated."
#endif

/* Steady-state RX buffer. Grows to CONFIG_ESPOS_SK_RX_FRAME_MAX for a big
 * frame, then shrinks straight back so a one-off burst is not a permanent
 * internal-RAM cost. */
#define RX_BUF_MIN 4096

/* TCP + TLS + WebSocket upgrade budget for one connect attempt. The error path
 * compares against it to tell a timeout apart from a refused or dropped
 * connection. */
#define SK_WS_CONNECT_TIMEOUT_MS 8000

static const char *TAG = "espos_skws";

static struct {
    TaskHandle_t task;
    SemaphoreHandle_t lock;          /* delta engine + meta table + status */
    espos_sk_delta_t *delta;
    espos_sk_meta_table_t meta;
    volatile bool stop;
    volatile bool cfg_dirty;
    /* config */
    bool enabled;
    uint32_t batch_ms, drain_per_s, health_ms, stall_ms;
    size_t buffer_msgs, buffer_bytes;
    /* status */
    espos_sk_ws_status_t st;
    bool sending;                    /* a delta message is being written; reported as pending */
    uint32_t connected_since_ms;
    uint32_t backoff_round;
    uint32_t retry_at_ms;
    char label[40];
} s;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }
static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

static void publish_status(void)
{
    char *json = espos_sk_ws_status_json();
    if (json) {
        espos_httpd_sse_publish("sk_ws", json);
        free(json);
    }
}

/* ------------------------------------------------------------ config */

static void load_cfg(void)
{
    bool en = true;
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_WS_ENABLED, &en);
    int32_t v = 100;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BATCH_MS, &v);
    uint32_t batch = (uint32_t)v;
    v = 128;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BUFFER_MSGS, &v);
    size_t msgs = (size_t)v;
    v = 32;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_BUFFER_KB, &v);
    size_t bytes = (size_t)v * 1024;
    v = 20;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DRAIN_PER_S, &v);
    uint32_t drain = (uint32_t)v;
    v = 10;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_HEALTH_S, &v);
    uint32_t health = (uint32_t)v * 1000;
    v = 300;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_STALL_S, &v);
    uint32_t stall = (uint32_t)v * 1000;
    /* The source label is the device's hostname, espos_net's (net.hostname,
     * default espos-<id>); one name on mDNS, in the access request and on
     * every delta. */
    char h[33] = { 0 };
    espos_net_status_t ns;
    if (espos_net_get_status(&ns) == ESP_OK) {
        snprintf(h, sizeof(h), "%s", ns.hostname);
    }
    lock();
    s.enabled = en;
    s.batch_ms = batch;
    s.drain_per_s = drain;
    s.health_ms = health;
    s.stall_ms = stall;
    if (h[0]) {
        snprintf(s.label, sizeof(s.label), "%s", h);
    } else {
        snprintf(s.label, sizeof(s.label), "espos-%s", espos_net_short_id());
    }
    /* Buffer geometry changes rebuild the engine (buffered messages are lost). */
    if (!s.delta || msgs != s.buffer_msgs || bytes != s.buffer_bytes) {
        s.buffer_msgs = msgs;
        s.buffer_bytes = bytes;
        espos_sk_delta_cfg_t c = { .label = s.label, .batch_ms = batch, .max_msgs = msgs, .max_bytes = bytes, .drain_per_s = drain };
        espos_sk_delta_t *nd = espos_sk_delta_create(&c);
        if (nd) {
            espos_sk_delta_destroy(s.delta);
            s.delta = nd;
        }
    } else {
        espos_sk_delta_set_label(s.delta, s.label);
        espos_sk_delta_set_timing(s.delta, batch, drain);
    }
    /* Stamp each value with the moment it was measured rather than the moment
     * it reaches the server: a message buffered through an outage would
     * otherwise land, an hour late, as if it had just happened. Re-applied on
     * every config load so toggling sk.timestamps takes effect without a
     * restart; the engine reads the clock itself when it drains. */
    if (s.delta) {
        bool ts = true;
        espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_TIMESTAMPS, &ts);
        espos_sk_delta_set_clock(s.delta, ts ? espos_time_now_ms_or_zero : NULL, NULL);
    }
    unlock();
}

void espos_sk_ws_config_changed(void)
{
    s.cfg_dirty = true;
}

/* ----------------------------------------------------------- publish */

static esp_err_t publish(const char *context, const char *path, const char *value_json, int64_t at_ms)
{
    if (!s.lock || !s.delta) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = espos_sk_delta_publish_at(s.delta, context, path, value_json, at_ms, now_ms());
    unlock();
    return err;
}

static esp_err_t publish_string(const char *context, const char *path, const char *value, int64_t at_ms)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    char v[ESPOS_SK_VALUE_MAX];
    if (espos_sk_json_string(v, sizeof(v), value) <= 0 || strlen(value) * 2 + 2 >= sizeof(v)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return publish(context, path, v, at_ms);
}

static esp_err_t publish_number(const char *context, const char *path, double value, int64_t at_ms)
{
    char v[32];
    espos_sk_json_number(v, sizeof(v), value);
    return publish(context, path, v, at_ms);
}

esp_err_t espos_sk_publish_number(const char *path, double value)
{
    return publish_number(NULL, path, value, 0);
}

esp_err_t espos_sk_publish_string(const char *path, const char *value)
{
    return publish_string(NULL, path, value, 0);
}

esp_err_t espos_sk_publish_bool(const char *path, bool value)
{
    return publish(NULL, path, value ? "true" : "false", 0);
}

esp_err_t espos_sk_publish_json(const char *path, const char *value_json)
{
    return publish(NULL, path, value_json, 0);
}

/* The _ctx calls insist on a context: NULL means vessels.self to the engine,
 * and another vessel's data landing on self is the one mistake here that
 * corrupts the boat's own model instead of failing. */
esp_err_t espos_sk_publish_number_ctx(const char *context, const char *path, double value)
{
    return espos_sk_publish_number_ctx_at(context, path, value, 0);
}

esp_err_t espos_sk_publish_string_ctx(const char *context, const char *path, const char *value)
{
    return espos_sk_publish_string_ctx_at(context, path, value, 0);
}

esp_err_t espos_sk_publish_bool_ctx(const char *context, const char *path, bool value)
{
    return espos_sk_publish_bool_ctx_at(context, path, value, 0);
}

esp_err_t espos_sk_publish_json_ctx(const char *context, const char *path, const char *value_json)
{
    return espos_sk_publish_json_ctx_at(context, path, value_json, 0);
}

esp_err_t espos_sk_publish_number_ctx_at(const char *context, const char *path, double value, int64_t at_ms)
{
    return context ? publish_number(context, path, value, at_ms) : ESP_ERR_INVALID_ARG;
}

esp_err_t espos_sk_publish_string_ctx_at(const char *context, const char *path, const char *value, int64_t at_ms)
{
    return context ? publish_string(context, path, value, at_ms) : ESP_ERR_INVALID_ARG;
}

esp_err_t espos_sk_publish_bool_ctx_at(const char *context, const char *path, bool value, int64_t at_ms)
{
    return context ? publish(context, path, value ? "true" : "false", at_ms) : ESP_ERR_INVALID_ARG;
}

esp_err_t espos_sk_publish_json_ctx_at(const char *context, const char *path, const char *value_json, int64_t at_ms)
{
    return context ? publish(context, path, value_json, at_ms) : ESP_ERR_INVALID_ARG;
}

esp_err_t espos_sk_set_no_drop(bool on)
{
    if (!s.lock || !s.delta) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    espos_sk_delta_set_no_drop(s.delta, on);
    unlock();
    return ESP_OK;
}

esp_err_t espos_sk_declare_meta(const char *path, const char *meta_json, uint32_t period_ms)
{
    if (!path || !*path || strlen(path) >= ESPOS_SK_PATH_MAX || !meta_json) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *j = cJSON_Parse(meta_json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return ESP_ERR_INVALID_ARG;
    }
    if (period_ms) {
        cJSON_DeleteItemFromObject(j, "timeout");
        cJSON_AddNumberToObject(j, "timeout", (double)period_ms * 2.5 / 1000.0);
    }
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
    }
    lock();
    esp_err_t err = espos_sk_meta_declare(&s.meta, path, txt, period_ms);
    unlock();
    return err;
}

/* Reconcile every not-yet-reconciled path: GET the server's meta, PUT ours
 * only if the server has none. Called by the client task (espos_sk.c), which
 * makes the other blocking HTTP calls too: on the stream task a server slow
 * to answer REST held the stream for one leg timeout per path, past its
 * watchdog. A round ends early on no answer, a server error or a refused
 * token (espos_sk_meta_after_*), and what is left waits for a backed-off
 * retry. */
void espos_sk_ws_meta_round(void)
{
    if (!s.lock) {
        return;
    }
    espos_sk_server_t srv = { 0 };
    char token[ESPOS_SK_TOKEN_MAX] = { 0 };
    /* The stream may still count as connected for a moment after the server
     * went: count that as a failed round, or the caller would spin on it. */
    espos_sk_meta_step_t stop = ESPOS_SK_META_DONE;
    if (espos_sk_get_server(&srv) != ESP_OK || espos_sk_get_token(token, sizeof(token)) != ESP_OK) {
        stop = ESPOS_SK_META_BACK_OFF;
    }
    lock();
    size_t n = s.meta.n;
    uint32_t epoch = s.meta.epoch;
    uint32_t conn = s.meta.conn;
    unlock();
    for (size_t k = 0; k < n && stop == ESPOS_SK_META_DONE; k++) {
        char path[ESPOS_SK_PATH_MAX];
        lock();
        size_t i = espos_sk_meta_index(&s.meta, n, k);
        const espos_sk_meta_entry_t *e = &s.meta.e[i];
        if (e->reconciled) {
            unlock();
            continue;
        }
        strcpy(path, e->path);
        char *meta = strdup(e->meta_json);
        uint32_t gen = e->gen;
        bool had_replaced = e->stale != NULL;
        char *replaced = had_replaced ? strdup(e->stale) : NULL;
        unlock();
        if (!meta || had_replaced != (replaced != NULL)) {
            /* Without the replaced text the GET would take it for the
             * server's own and keep it: try again after a backoff. */
            free(meta);
            free(replaced);
            lock();
            espos_sk_meta_leg_done(&s.meta, i, gen, ESPOS_SK_META_BACK_OFF);
            unlock();
            stop = ESPOS_SK_META_BACK_OFF;
            continue;
        }
        espos_sk_meta_seen_t seen;
        int st = espos_sk_http_get_meta(&srv, token, path, replaced, &seen);
        free(replaced);
        espos_sk_meta_step_t step = espos_sk_meta_after_get(st, seen);
        if (step == ESPOS_SK_META_DONE) {
            ESP_LOGI(TAG, "meta %s: server has its own%s, keeping it", path,
                     seen == ESPOS_SK_META_UNREADABLE ? " (too large or not JSON to read)" : "");
        } else if (step == ESPOS_SK_META_PUT) {
            /* Redeclared during the GET: the copy is stale, and once on the
             * server the next round would keep it as the server's own. That
             * round asks again and sends the current text. The lock is not
             * held across the PUT, which can take a leg timeout. */
            lock();
            bool stale = s.meta.e[i].gen != gen;
            unlock();
            int put = 0;
            step = ESPOS_SK_META_LATER;
            if (!stale) {
                put = espos_sk_http_put_meta(&srv, token, path, meta);
                step = espos_sk_meta_after_put(put);
            }
            if (step == ESPOS_SK_META_DONE) {
                lock();
                espos_sk_meta_sent(&s.meta, i, gen, conn, meta);
                unlock();
                meta = NULL;
            }
            if (stale) {
                ESP_LOGI(TAG, "meta %s: redeclared meanwhile, next round", path);
            } else if (step == ESPOS_SK_META_DONE) {
                ESP_LOGI(TAG, "meta %s: sent", path);
            } else {
                ESP_LOGW(TAG, "meta %s: PUT → %d", path, put);
            }
        } else {
            ESP_LOGW(TAG, "meta %s: GET → %d", path, st);
        }
        free(meta);
        lock();
        espos_sk_meta_leg_done(&s.meta, i, gen, step);
        unlock();
        if (step == ESPOS_SK_META_UNAUTHORIZED || step == ESPOS_SK_META_BACK_OFF) {
            stop = step;
        }
    }
    if (stop == ESPOS_SK_META_UNAUTHORIZED) {
        espos_sk_report_unauthorized();
    }
    /* A declaration or a reconnect while the round ran asks for a fresh
     * round, not for this one's backoff. */
    lock();
    uint32_t d = s.meta.epoch != epoch
                     ? 0
                     : espos_sk_meta_round_end(&s.meta, now_ms(), espos_net_backoff_ms(s.meta.round, 300000, (uint32_t)rand()));
    unlock();
    if (d) {
        ESP_LOGW(TAG, "meta: %s, next round in %u s",
                 stop == ESPOS_SK_META_UNAUTHORIZED ? "token refused"
                 : stop == ESPOS_SK_META_BACK_OFF   ? "no answer or a server error"
                                                    : "paths left unreconciled",
                 (unsigned)(d / 1000));
    }
}

uint32_t espos_sk_ws_meta_wait_ms(void)
{
    if (!s.lock) {
        return UINT32_MAX;
    }
    lock();
    uint32_t wait = espos_sk_meta_wait(&s.meta, s.st.connected, now_ms());
    unlock();
    return wait;
}

/* ------------------------------------------------------------ health */

#if CONFIG_ESPOS_SK_NOTIFICATIONS

/* Conditions themselves live in espos_health: it owns the per-key state and
 * the level-triggered dedup, and knows nothing about SignalK. What is left
 * here is one sink that turns a change into a delta -- so a component with a
 * condition to report (espos_voice's wake service, an application's bus
 * timeout) depends on espos_health and not on the whole SignalK stack.
 *
 * The two enums are the same three levels in the same order, and stay that
 * way: espos_sk_notify() is public API and forwards straight through. */
_Static_assert((int)ESPOS_SK_ALERT_NORMAL == (int)ESPOS_HEALTH_NORMAL, "alert/health enums diverged");
_Static_assert((int)ESPOS_SK_ALERT_WARN == (int)ESPOS_HEALTH_WARN, "alert/health enums diverged");
_Static_assert((int)ESPOS_SK_ALERT_ALARM == (int)ESPOS_HEALTH_ALARM, "alert/health enums diverged");

/* Set once the sink is registered, which espos_sk_ws_start() does. */
static volatile bool s_notify_ready;

/* The sink's arg for a replay, which it logs more quietly than a change. The
 * registered sink's arg stays NULL. */
static char s_replay_tag;
#define HEALTH_REPLAY ((void *)&s_replay_tag)

/* Only ws_task replays with HEALTH_REPLAY, so only ws_task touches these. A
 * replay whose deltas could not all be queued is retried while connected,
 * rather than waiting for a reconnect that may not come for days. */
static bool s_replay_lost;
static bool s_restate_pending;
static uint32_t s_restate_at_ms;
#define RESTATE_RETRY_MS 5000

/* Runs on whichever task reported the condition, with no espos_health lock
 * held. Returns void: a sink cannot fail usefully, and a delta that cannot be
 * published now is the buffer's problem, not the reporter's. */
static void health_sink(const char *key, espos_health_state_t state,
                        const char *message, void *arg)
{
    /* Stopped: a replay already running on ws_task must not publish into it. */
    if (!s_notify_ready) return;
    const char *st = espos_health_state_str(state);

    char path[ESPOS_SK_PATH_MAX];
    if (snprintf(path, sizeof(path), "notifications.espos.%s.%s", s.label, key) >= (int)sizeof(path)) {
        ESP_LOGE(TAG, "condition '%s' does not fit a path", key);
        return;   /* a clipped path is the wrong path */
    }

    /* Built with cJSON rather than snprintf: the message is escaped properly,
     * and a value too long to serialise fails here instead of emitting JSON
     * that is silently truncated mid-string. */
    cJSON *v = cJSON_CreateObject();
    if (!v) return;
    cJSON_AddStringToObject(v, "state", st);
    cJSON_AddStringToObject(v, "message", message);
    cJSON *m = cJSON_AddArrayToObject(v, "method");
    /* method is the server's business; the device only states the condition. */
    if (m && state != ESPOS_HEALTH_NORMAL) {
        cJSON_AddItemToArray(m, cJSON_CreateString("visual"));
    }
    char *val = cJSON_PrintUnformatted(v);
    cJSON_Delete(v);
    if (!val) return;

    /* A replay restates every condition on each connect; logging each one
     * would push the lines that explain a flapping link out of the log ring. */
    if (arg == HEALTH_REPLAY) {
        ESP_LOGD(TAG, "notification %s: %s (%s), restated", key, st, message);
    } else {
        ESP_LOGI(TAG, "notification %s: %s (%s)", key, st, message);
    }
    esp_err_t err = espos_sk_publish_json(path, val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notification %s not queued: %s", key, esp_err_to_name(err));
        if (arg == HEALTH_REPLAY) {
            s_replay_lost = true;
        }
    }
    cJSON_free(val);
}

/* A condition reaches the server only when it changes, so whatever the server
 * lost since stays lost: a clear dropped from a full offline buffer leaves a
 * warning there for good, and a server restart forgets a warning that is
 * still true. Called on every connect and on a config reload while
 * connected, and again every RESTATE_RETRY_MS while a previous call could not
 * queue everything. Queued deltas drain first, so the current state lands
 * last. */
static void restate_conditions(void)
{
    if (!s_notify_ready) return;
    s_replay_lost = false;
    esp_err_t err = espos_health_replay(health_sink, HEALTH_REPLAY);
    if (err == ESP_OK && !s_replay_lost) {
        s_restate_pending = false;
        ESP_LOGI(TAG, "health conditions restated");
        return;
    }
    if (!s_restate_pending) {
        ESP_LOGW(TAG, "health conditions not all restated (%s); retrying",
                 err != ESP_OK ? esp_err_to_name(err) : "queue full");
    }
    s_restate_pending = true;
    s_restate_at_ms = now_ms() + RESTATE_RETRY_MS;
}

esp_err_t espos_sk_notify(const char *key, espos_sk_alert_t state, const char *message)
{
    /* Still an error before espos_sk_start(), even though espos_health would
     * happily record it: a caller reaching for the SignalK spelling of this
     * wants a delta, and silently accepting one that can never be published
     * is how a device ends up looking healthy because nothing was listening.
     * Report conditions through espos_health_report() if you want them
     * recorded regardless of whether SignalK is up. */
    if (!s_notify_ready) return ESP_ERR_INVALID_STATE;
    return espos_health_report(key, (espos_health_state_t)state, message);
}

#else  /* !CONFIG_ESPOS_SK_NOTIFICATIONS */

esp_err_t espos_sk_notify(const char *key, espos_sk_alert_t state, const char *message)
{
    (void)key;
    (void)state;
    (void)message;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

/* Not now_ms() / 1000: that is the 32-bit tick count in milliseconds, which
 * wraps after 49.7 days and would report a boat's long-running device as
 * freshly booted. */
static int64_t uptime_s(void)
{
#if CONFIG_IDF_TARGET_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
#else
    return esp_timer_get_time() / 1000000;
#endif
}

static void publish_health(void)
{
    char base[64];
    snprintf(base, sizeof(base), "espos.%s.", s.label);
    static bool declared;
    if (!declared) {
        declared = true;
        char p[ESPOS_SK_PATH_MAX];
        uint32_t period = s.health_ms;
        snprintf(p, sizeof(p), "%suptime", base);
        espos_sk_declare_meta(p, "{\"units\":\"s\",\"description\":\"Time since boot\"}", period);
        snprintf(p, sizeof(p), "%sfreeHeap", base);
        espos_sk_declare_meta(p, "{\"description\":\"Free heap (bytes)\"}", period);
        snprintf(p, sizeof(p), "%sminFreeHeap", base);
        espos_sk_declare_meta(p, "{\"description\":\"Minimum free heap since boot (bytes)\"}", period);
        snprintf(p, sizeof(p), "%sinternalFree", base);
        espos_sk_declare_meta(p, "{\"description\":\"Free internal RAM (bytes)\"}", period);
        snprintf(p, sizeof(p), "%slargestBlock", base);
        espos_sk_declare_meta(p, "{\"description\":\"Largest free internal RAM block (bytes)\"}", period);
        snprintf(p, sizeof(p), "%srssi", base);
        espos_sk_declare_meta(p, "{\"units\":\"dB\",\"description\":\"WiFi RSSI\"}", period);
        snprintf(p, sizeof(p), "%swifiReconnects", base);
        espos_sk_declare_meta(p, "{\"description\":\"WiFi reconnects since boot\"}", period);
        snprintf(p, sizeof(p), "%sskReconnects", base);
        espos_sk_declare_meta(p, "{\"description\":\"SignalK stream reconnects since boot\"}", period);
        snprintf(p, sizeof(p), "%sresetReason", base);
        espos_sk_declare_meta(p, "{\"description\":\"Reason of the last reset\"}", period);
    }
    char p[ESPOS_SK_PATH_MAX];
    snprintf(p, sizeof(p), "%suptime", base);
    espos_sk_publish_number(p, (double)uptime_s());
    snprintf(p, sizeof(p), "%sfreeHeap", base);
    espos_sk_publish_number(p, esp_get_free_heap_size());
    snprintf(p, sizeof(p), "%sminFreeHeap", base);
    espos_sk_publish_number(p, esp_get_minimum_free_heap_size());
    /* Internal RAM separately from the total: on a PSRAM board it is the
     * scarce pool, and the total hides it. Telemetry only -- the thresholds
     * and the lowMemory condition live in espos_health's policy, which runs
     * with or without SignalK. */
    snprintf(p, sizeof(p), "%sinternalFree", base);
    espos_sk_publish_number(p, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snprintf(p, sizeof(p), "%slargestBlock", base);
    espos_sk_publish_number(p, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* From the network seam, not the radio: rssi is 0 when the route is not
     * WiFi, and the path keeps its name (wifiReconnects) so dashboards built
     * on it keep working — it counts default-route re-establishments now,
     * which on a WiFi-only device is the same number. */
    espos_net_status_t ns;
    if (espos_net_get_status(&ns) == ESP_OK) {
        snprintf(p, sizeof(p), "%srssi", base);
        espos_sk_publish_number(p, ns.rssi);
        snprintf(p, sizeof(p), "%swifiReconnects", base);
        espos_sk_publish_number(p, ns.up_count > 0 ? ns.up_count - 1 : 0);
    }
    snprintf(p, sizeof(p), "%sskReconnects", base);
    espos_sk_publish_number(p, s.st.reconnects > 0 ? s.st.reconnects - 1 : 0);

    snprintf(p, sizeof(p), "%sresetReason", base);
    static const char *const reasons[] = { "unknown", "poweron", "external", "software", "panic", "int_wdt",
                                           "task_wdt", "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag",
                                           "efuse", "power_glitch", "cpu_lockup" };
    int r = (int)esp_reset_reason();
    espos_sk_publish_string(p, (r >= 0 && r < (int)(sizeof(reasons) / sizeof(reasons[0]))) ? reasons[r] : "unknown");
}

/* ------------------------------------------------------------- stream */

/* The inbound side and the event bus learn about the stream together, so a
 * subscriber never sees SK_STREAM_CONNECTED while espos_sk_put() would still
 * refuse. Runs on the ws task. */
static void set_stream_connected(bool up)
{
    espos_sk_inbound_set_connected(up);
    (void)espos_event_post(up ? ESPOS_EVENT_SK_STREAM_CONNECTED : ESPOS_EVENT_SK_STREAM_DISCONNECTED, NULL, 0);
}

/* skLinkStalled: the device is trying (enabled, WiFi up, server and token in
 * hand), the stream has worked at least once this boot, and it has now been
 * down for sk.stall_s. The WiFi state machine cannot see a wedged radio link
 * -- on a co-processor board the disconnect event never crosses the jammed
 * transport, so it keeps reporting CONNECTED while nothing reaches the
 * network. The stream is real traffic over that same link and drops within
 * seconds of a wedge, which makes "connected, yet no stream for minutes" the
 * honest signal; it is also the one network condition a restart fixes, hence
 * the fatal flag. A plain WiFi loss is never reported here (espos_core's
 * netDown, a warning) and a server that is merely down for a while is covered
 * by the generous default -- raise sk.stall_s if servers reboot slowly. */
static void check_stall(bool trying, bool connected, uint32_t *down_since_ms)
{
    lock();
    bool ever = s.st.reconnects > 0;
    uint32_t stall_ms = s.stall_ms;
    unlock();
    if (!trying || !ever || connected) {
        *down_since_ms = 0;
        espos_health_report_ex("skLinkStalled", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
        return;
    }
    uint32_t t = now_ms();
    if (!*down_since_ms) {
        *down_since_ms = t ? t : 1;
        return;
    }
    if (t - *down_since_ms >= stall_ms) {
        /* The threshold, not the running count: a message that changes every
         * second is a delta every second into a buffer the stall keeps full. */
        char m[ESPOS_HEALTH_MSG_MAX];
        snprintf(m, sizeof(m), "stream down for over %u s while WiFi reports connected", (unsigned)(stall_ms / 1000));
        espos_health_report_ex("skLinkStalled", ESPOS_HEALTH_ALARM, m, ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    }
}

/* -------------------------------------------------------------- task */

static void set_error(const char *msg)
{
    lock();
    snprintf(s.st.last_error, sizeof(s.st.last_error), "%s", msg ? msg : "");
    unlock();
}

static void ws_task(void *arg)
{
    (void)arg;
    esp_transport_handle_t tcp = NULL, ws = NULL;
    bool connected = false;
    uint32_t next_health = 0;
    uint32_t down_since_ms = 0;
    size_t cap = RX_BUF_MIN;
    char *buf = malloc(cap);
    char headers[ESPOS_SK_TOKEN_MAX + 32];
    espos_sk_server_t cur_srv = { 0 };
    char cur_token[ESPOS_SK_TOKEN_MAX] = { 0 };

    /* The loop never blocks longer than the 8 s connect timeout, so a 20 s
     * budget separates "slow network" from "wedged task" without false
     * alarms; a wedged stream task is exactly the failure the health policy
     * exists to name and recover from. */
    espos_health_watch_task("espos_skws", 20000);

    while (!s.stop) {
        espos_health_kick();
        if (s.cfg_dirty) {
            s.cfg_dirty = false;
            load_cfg();
#if CONFIG_ESPOS_SK_NOTIFICATIONS
            /* The stream stays up across a reload, yet a rename moves every
             * notification path and a new buffer size drops what was queued. */
            if (connected) {
                restate_conditions();
            }
#endif
        }
        /* preconditions: enabled, network up, server + token */
        espos_sk_server_t srv;
        char token[ESPOS_SK_TOKEN_MAX];
        bool net_up = espos_net_is_up();
        /* token "" is fine when the server runs without security (OPEN); the
         * token machine only exposes a server once it is usable */
        bool have = espos_sk_get_server(&srv) == ESP_OK && espos_sk_get_token(token, sizeof(token)) == ESP_OK &&
                    espos_sk_stream_allowed();
        bool ready = s.enabled && net_up && have;
        check_stall(ready, connected, &down_since_ms);
        if (connected) {
            bool changed = strcmp(srv.host, cur_srv.host) != 0 || srv.port != cur_srv.port ||
                           srv.tls != cur_srv.tls || strcmp(token, cur_token) != 0;
            if (!ready || changed) {
                ESP_LOGI(TAG, "closing stream (%s)", !s.enabled ? "disabled" : !net_up ? "network down"
                                                                           : changed   ? "server/token changed"
                                                                                       : "no server");
                esp_transport_close(ws);
                connected = false;
                set_stream_connected(false);
                lock();
                s.st.connected = false;
                if (s.delta) {
                    espos_sk_delta_flush(s.delta, now_ms());
                }
                unlock();
                publish_status();
            }
        }
        if (!connected) {
            /* offline: still close batch windows so values land in the ring
             * (a stale path keeps its history instead of coalescing forever) */
            uint32_t idle = 250;
            lock();
            if (s.delta) {
                free(espos_sk_delta_take(s.delta, now_ms(), false));
                uint32_t due = espos_sk_delta_next_due_ms(s.delta, now_ms(), false);
                if (due < idle) {
                    idle = due ? due : 1;
                }
            }
            unlock();
            if (!ready) {
                lock();
                s.st.next_retry_s = 0;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(idle));
                continue;
            }
            uint32_t t = now_ms();
            if (s.retry_at_ms && (int32_t)(s.retry_at_ms - t) > 0) {
                lock();
                s.st.next_retry_s = (s.retry_at_ms - t) / 1000;
                unlock();
                vTaskDelay(pdMS_TO_TICKS(idle));
                continue;
            }
            /* connect. A transport is reused across reconnects, and
             * esp_transport_ws keeps the previous attempt's HTTP upgrade
             * status in it; build a fresh pair every time so the status the
             * error path reads belongs to this attempt. It also makes the
             * ws-vs-wss choice follow discovery after a scheme change, which
             * is why sk.scheme is not restart_required. */
            if (ws != NULL) {
                /* ws_destroy() frees only the WebSocket layer; the child
                 * TCP/TLS transport is a separate handle and must go too. */
                esp_transport_destroy(ws);
                if (tcp != NULL) {
                    esp_transport_destroy(tcp);
                }
                ws = NULL;
                tcp = NULL;
            }
            if (!tcp) {
#if CONFIG_ESPOS_SK_TLS
                if (srv.tls) {
                    tcp = esp_transport_ssl_init();
                    /* The trust store, not the Mozilla bundle -- the same
                     * decision the HTTP legs make, through the same callback
                     * and the same single anchor. */
                    esp_transport_ssl_crt_bundle_attach(tcp, espos_sk_tls_attach);
                    if (espos_sk_tls_trust_mode() != ESPOS_SK_TLS_TRUST_BUNDLE) {
                        /* Already decided by fingerprint and SAN set; a boat
                         * server is reached by IP as often as by name and the
                         * CN check fails on that alone. */
                        esp_transport_ssl_skip_common_name_check(tcp);
                    }
                } else {
                    tcp = esp_transport_tcp_init();
                }
#else
                tcp = esp_transport_tcp_init();
#endif
                ws = tcp ? esp_transport_ws_init(tcp) : NULL;
                /* A failed set leaves IDF's default User-Agent in place, so
                 * it fails the attempt rather than dialling without ours. */
                if (!ws || esp_transport_ws_set_user_agent(ws, espos_net_user_agent()) != ESP_OK) {
                    if (ws) esp_transport_destroy(ws);
                    if (tcp) esp_transport_destroy(tcp);
                    ws = NULL;
                    tcp = NULL;
                    set_error("out of memory for the WebSocket transport");
                    s.retry_at_ms = now_ms() + 1000;
                    publish_status();
                    continue;
                }
            }
            esp_transport_ws_set_path(ws, "/signalk/v1/stream?subscribe=none&sendMeta=all");
            if (token[0]) {
                snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
            } else {
                headers[0] = '\0';
            }
            esp_transport_ws_set_headers(ws, headers[0] ? headers : NULL);
            ESP_LOGI(TAG, "connecting to %s://%s:%u/signalk/v1/stream (subscribe=none, sendMeta=all)",
                     srv.tls ? "wss" : "ws", srv.host, srv.port);
#if CONFIG_ESPOS_SK_TLS
            if (srv.tls) {
                /* One handshake at a time, device-wide, and a pre-flight
                 * memory check: the token machine's HTTP legs take the same
                 * slot, and this is the reconnect loop that would otherwise
                 * meet them head on. */
                esp_err_t hs = espos_sk_tls_handshake_begin(CONFIG_ESPOS_SK_TLS_HANDSHAKE_TIMEOUT_MS);
                if (hs != ESP_OK) {
                    set_error(hs == ESP_ERR_NO_MEM ? "waiting for memory for a TLS handshake"
                                                   : "waiting for the TLS handshake slot");
                    s.retry_at_ms = now_ms() + 5000;
                    publish_status();
                    continue;
                }
            }
#endif
            const uint32_t connect_started_ms = now_ms();
            const int rc = esp_transport_connect(ws, srv.host, srv.port,
                                                 SK_WS_CONNECT_TIMEOUT_MS);
            const int64_t connect_ms =
                (int64_t)(uint32_t)(now_ms() - connect_started_ms);
            /* Read the transport's captured socket error before the close below
             * clears it; the global errno may belong to another task. */
            const int transport_errno = esp_transport_get_errno(tcp);
            const int http =
                rc < 0 ? esp_transport_ws_get_upgrade_request_status(ws) : 101;
#if CONFIG_ESPOS_SK_TLS
            /* Capture the verify callback's reason before the discard below:
             * handshake_begin() cleared it, so an empty string here means this
             * attempt failed before (or without) a certificate refusal. */
            char cert_reason[96] = { 0 };
            if (srv.tls && espos_sk_tls_last_error()[0]) {
                snprintf(cert_reason, sizeof(cert_reason), "%s",
                         espos_sk_tls_last_error());
            }
            if (srv.tls) {
                /* 101 is the only proof this device has that the other end is
                 * a SignalK server rather than something that merely finished
                 * a handshake -- so it is the only thing that commits an
                 * anchor. A refused upgrade discards whatever was captured. */
                if (rc >= 0) {
                    espos_sk_tls_commit(srv.self[0] ? srv.self : NULL);
                } else {
                    espos_sk_tls_discard();
                }
                espos_sk_tls_handshake_end();
            }
#endif
            if (rc < 0) {
                esp_transport_close(ws);
                char msg[128];
#if CONFIG_ESPOS_SK_TLS
                if (cert_reason[0]) {
                    /* Not "connect failed": the token machine has a state for
                     * this that keeps the token and retries on a flat minute
                     * instead of an exponential backoff. */
                    snprintf(msg, sizeof(msg), "%s", cert_reason);
                    espos_sk_report_cert_error(msg);
                    ESP_LOGW(TAG, "%s", msg);
                    set_error(msg);
                    s.retry_at_ms = now_ms() + 30000;
                    publish_status();
                    continue;
                }
#endif
                if (http == 401 || http == 403) {
                    snprintf(msg, sizeof(msg),
                             "stream refused (HTTP %d): token rejected", http);
                    espos_sk_report_unauthorized();
                } else if (http == 101) {
                    /* The upgrade response arrived (101) but the WebSocket
                     * handshake itself did not complete: a bad accept, an
                     * immediate close, or a proxy that answered but did not
                     * carry the stream. */
                    snprintf(msg, sizeof(msg),
                             "websocket upgrade failed (HTTP 101 after %lld ms)",
                             (long long)connect_ms);
                } else if (http > 0) {
                    /* An HTTP refusal is a response; it must win over the
                     * elapsed-time timeout, which can also be reached while
                     * the server is answering. */
                    snprintf(msg, sizeof(msg),
                             "stream refused (HTTP %d) after %lld ms", http,
                             (long long)connect_ms);
                } else if (connect_ms >= SK_WS_CONNECT_TIMEOUT_MS) {
                    /* No HTTP response at all: TCP or TLS ran into the connect
                     * budget. This used to be reported as the previous
                     * attempt's stale "HTTP 101". */
                    snprintf(msg, sizeof(msg),
                             "connect timeout after %lld ms (no upgrade response)",
                             (long long)connect_ms);
                } else {
                    snprintf(msg, sizeof(msg),
                             "connect failed after %lld ms (transport errno %d: %s)",
                             (long long)connect_ms, transport_errno,
                             transport_errno ? strerror(transport_errno)
                                             : "no error");
                }
                ESP_LOGW(TAG, "%s", msg);
                set_error(msg);
                uint32_t d = espos_net_backoff_ms(s.backoff_round, 60000, (uint32_t)rand());
                if (s.backoff_round < 30) {
                    s.backoff_round++;
                }
                s.retry_at_ms = now_ms() + d;
                if (!s.retry_at_ms) {
                    s.retry_at_ms = 1;
                }
                publish_status();
                continue;
            }
            connected = true;
            cur_srv = srv;
            strcpy(cur_token, token);
            s.backoff_round = 0;
            s.retry_at_ms = 0;
            lock();
            s.st.connected = true;
            s.st.reconnects++;
            s.st.next_retry_s = 0;
            espos_sk_meta_connected(&s.meta); /* the client task reconciles at once */
            s.st.last_error[0] = '\0';
            s.connected_since_ms = now_ms();
            unlock();
            ESP_LOGI(TAG, "stream connected");
            set_stream_connected(true);
            publish_status();
#if CONFIG_ESPOS_SK_NOTIFICATIONS
            restate_conditions();
#endif
        }

        /* connected: send what is due, read what comes in */
        uint32_t t = now_ms();
        if (s.health_ms && (int32_t)(t - next_health) >= 0) {
            publish_health();
            next_health = t + s.health_ms;
        }
#if CONFIG_ESPOS_SK_NOTIFICATIONS
        if (s_restate_pending && (int32_t)(t - s_restate_at_ms) >= 0) {
            restate_conditions();
        }
#endif
        espos_sk_inbound_tick(t);
        /* control frames first: subscriptions, PUTs, raw sends */
        char *frame = espos_sk_inbound_take_frame();
        if (frame) {
            ESP_LOGD(TAG, "tx %.160s", frame);
            int w = esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, frame, (int)strlen(frame), 3000);
            free(frame);
            if (w < 0) {
                ESP_LOGW(TAG, "send failed; reconnecting");
                lock();
                s.st.send_errors++;
                s.st.connected = false;
                unlock();
                set_error("send failed");
                esp_transport_close(ws);
                connected = false;
                set_stream_connected(false);
                s.retry_at_ms = now_ms() + 1000;
                publish_status();
            }
            continue;
        }
        char *msg = NULL;
        lock();
        if (s.delta) {
            msg = espos_sk_delta_take(s.delta, t, true);
        }
        /* Off the queue but not yet written. Counted nowhere, this message
         * let espos_sk_flush() report a drained stream for up to the send
         * timeout while its last frame was still going out -- or about to
         * fail and be requeued after the caller had gone to sleep. */
        s.sending = msg != NULL;
        unlock();
        if (msg) {
            int w = esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN, msg, (int)strlen(msg), 3000);
            if (w < 0) {
                ESP_LOGW(TAG, "send failed; reconnecting");
                lock();
                espos_sk_delta_requeue(s.delta, msg);
                s.sending = false;
                s.st.send_errors++;
                s.st.connected = false;
                unlock();
                set_error("send failed");
                esp_transport_close(ws);
                connected = false;
                set_stream_connected(false);
                s.retry_at_ms = now_ms() + 1000;
                publish_status();
                continue;
            }
            free(msg);
            lock();
            s.sending = false;
            s.st.sent++;
            unlock();
            continue; /* look for more right away */
        }
        /* nothing to send: wait for input or the next deadline */
        uint32_t wait = 250;
        lock();
        uint32_t due = s.delta ? espos_sk_delta_next_due_ms(s.delta, t, true) : UINT32_MAX;
        unlock();
        if (due < wait) {
            wait = due;
        }
        if (s.health_ms) {
            int32_t hl = (int32_t)(next_health - t);
            if (hl >= 0 && (uint32_t)hl < wait) {
                wait = (uint32_t)hl;
            }
        }
        int pr = esp_transport_poll_read(ws, (int)wait);
        if (pr > 0) {
            /* Read one whole text frame: esp_transport_ws hands payload
             * back in buffer-sized pieces, so gather until payload_len. */
            size_t have = 0;
            bool broken = false, complete = false;
            while (!complete && !broken) {
                if (have + 1 >= cap) {
                    if (cap >= CONFIG_ESPOS_SK_RX_FRAME_MAX) {
                        ESP_LOGW(TAG, "frame larger than %d bytes dropped", CONFIG_ESPOS_SK_RX_FRAME_MAX);
                        /* drain the rest of this frame */
                        int r = esp_transport_read(ws, buf, (int)cap - 1, 100);
                        if (r <= 0) {
                            broken = r < 0;
                            break;
                        }
                        have = 0;
                        if (esp_transport_ws_get_read_payload_len(ws) <= r) {
                            complete = true;   /* nothing useful in buf */
                            have = 0;
                        }
                        continue;
                    }
                    size_t ncap = cap * 2 > (size_t)CONFIG_ESPOS_SK_RX_FRAME_MAX ? (size_t)CONFIG_ESPOS_SK_RX_FRAME_MAX : cap * 2;
                    char *nb = realloc(buf, ncap);
                    if (!nb) {
                        broken = true;
                        break;
                    }
                    buf = nb;
                    cap = ncap;
                }
                int r = esp_transport_read(ws, buf + have, (int)(cap - have - 1), have ? 1000 : 100);
                if (r < 0) {
                    broken = true;
                    break;
                }
                if (r == 0) {
                    if (have == 0) {
                        break;      /* control frame (ping/pong) handled by the transport */
                    }
                    continue;
                }
                have += (size_t)r;
                int total = esp_transport_ws_get_read_payload_len(ws);
                if ((int)have >= total && esp_transport_ws_get_fin_flag(ws)) {
                    complete = true;
                } else if ((int)have >= total) {
                    /* fragmented message: keep appending the next fragment */
                    continue;
                }
            }
            if (complete && have) {
                buf[have] = '\0';
                ESP_LOGD(TAG, "rx %u bytes: %.200s", (unsigned)have, buf);
                char err[64];
                if (!espos_sk_inbound_handle_frame(buf, have, err, sizeof(err))) {
                    ESP_LOGW(TAG, "server: %s", err);
                    lock();
                    snprintf(s.st.last_error, sizeof(s.st.last_error), "%.60s", err);
                    unlock();
                }
                /* Give the growth back. One oversized frame — a server's
                 * notification backfill at connect is the usual one — would
                 * otherwise pin the buffer at its peak for the life of the
                 * connection. On boards where internal RAM is the scarce
                 * resource (PSRAM makes the total look healthy while the
                 * largest free internal block is what actually runs out)
                 * that is a permanent cost paid for a transient burst.
                 * Shrink failures are ignored: keeping the larger buffer is
                 * correct behaviour, not an error. */
                if (cap > RX_BUF_MIN) {
                    char *sb = realloc(buf, RX_BUF_MIN);
                    if (sb) {
                        buf = sb;
                        cap = RX_BUF_MIN;
                    }
                }
            }
            if (broken) {
                ESP_LOGW(TAG, "stream closed by server");
                lock();
                s.st.connected = false;
                unlock();
                set_error("closed by server");
                esp_transport_close(ws);
                connected = false;
                set_stream_connected(false);
                s.retry_at_ms = now_ms() + 1000;
                publish_status();
            }
        } else if (pr < 0) {
            lock();
            s.st.connected = false;
            unlock();
            set_error("connection error");
            esp_transport_close(ws);
            connected = false;
            set_stream_connected(false);
            s.retry_at_ms = now_ms() + 1000;
            publish_status();
        }
    }
    if (connected) {
        esp_transport_close(ws);
        set_stream_connected(false);
    }
    if (ws) {
        esp_transport_destroy(ws);
    }
    if (tcp) {
        esp_transport_destroy(tcp);
    }
    free(buf);
    /* A task still registered with the task watchdog after it exits is
     * exactly what trips the TWDT; leave the registry before the handle dies. */
    espos_health_unwatch_task();
    s.task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ status */

esp_err_t espos_sk_flush(uint32_t timeout_ms)
{
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    bool connected = s.st.connected;
    /* Close the current batch so what was just published becomes a message
     * the stream task can take, instead of waiting out sk.batch_ms. */
    if (s.delta && connected) {
        espos_sk_delta_flush(s.delta, now_ms());
    }
    unlock();
    if (!connected) {
        /* Nothing can drain while the stream is down, so waiting would only
         * burn the caller's timeout before it sleeps. */
        return ESP_ERR_INVALID_STATE;
    }

    /* Poll rather than signal: the stream task is already looping over the
     * same queues, and a condition variable here would have to be taken on
     * its hot path to save a few 10 ms ticks on a call that happens once
     * before sleeping. */
    uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        espos_sk_ws_status_t st;
        if (espos_sk_ws_get_status(&st) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!st.connected) {
            return ESP_ERR_INVALID_STATE;
        }
        if (st.pending == 0 && st.buffered == 0) {
            return ESP_OK;
        }
        if ((int32_t)(now_ms() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t espos_sk_ws_get_status(espos_sk_ws_status_t *out)
{
    if (!out || !s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    *out = s.st;
    out->enabled = s.enabled;
    if (s.st.connected) {
        out->connected_s = (now_ms() - s.connected_since_ms) / 1000;
    }
    if (s.delta) {
        espos_sk_delta_stats_t ds;
        espos_sk_delta_stats(s.delta, &ds);
        out->pending = ds.pending + (s.sending ? 1 : 0);
        out->buffered = ds.buffered;
        out->buffered_bytes = ds.buffered_bytes;
        out->dropped = ds.dropped;
    }
    out->meta_declared = s.meta.n;
    out->meta_reconciled = 0;
    for (size_t i = 0; i < s.meta.n; i++) {
        out->meta_reconciled += s.meta.e[i].reconciled;
    }
    unlock();
    espos_sk_inbound_stats(out);
    return ESP_OK;
}

char *espos_sk_ws_status_json(void)
{
    espos_sk_ws_status_t st;
    if (espos_sk_ws_get_status(&st) != ESP_OK) {
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", st.enabled);
    cJSON_AddBoolToObject(j, "connected", st.connected);
    if (st.connected) {
        cJSON_AddNumberToObject(j, "connected_s", st.connected_s);
    } else if (st.next_retry_s) {
        cJSON_AddNumberToObject(j, "next_retry_s", st.next_retry_s);
    }
    cJSON_AddNumberToObject(j, "reconnects", st.reconnects);
    cJSON_AddNumberToObject(j, "sent", st.sent);
    cJSON_AddNumberToObject(j, "send_errors", st.send_errors);
    cJSON_AddNumberToObject(j, "pending", (double)st.pending);
    cJSON_AddNumberToObject(j, "buffered", (double)st.buffered);
    cJSON_AddNumberToObject(j, "buffered_bytes", (double)st.buffered_bytes);
    cJSON_AddNumberToObject(j, "dropped", st.dropped);
    cJSON_AddStringToObject(j, "last_error", st.last_error);
    cJSON *m = cJSON_AddObjectToObject(j, "meta");
    cJSON_AddNumberToObject(m, "declared", (double)st.meta_declared);
    cJSON_AddNumberToObject(m, "reconciled", (double)st.meta_reconciled);
    cJSON *in = cJSON_AddObjectToObject(j, "in");
    cJSON_AddNumberToObject(in, "subs", (double)st.subs);
    cJSON_AddNumberToObject(in, "frames", st.frames);
    cJSON_AddNumberToObject(in, "received", st.received);
    cJSON *pu = cJSON_AddObjectToObject(j, "put");
    cJSON_AddNumberToObject(pu, "pending", (double)st.puts_pending);
    cJSON_AddNumberToObject(pu, "ok", st.puts_sent);
    cJSON_AddNumberToObject(pu, "failed", st.puts_failed);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return txt;
}

/* --------------------------------------------------------- lifecycle */

esp_err_t espos_sk_ws_start(void)
{
    if (s.task) {
        return ESP_OK;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_cfg();
    s.stop = false;
    if (xTaskCreate(ws_task, "espos_skws", CONFIG_ESPOS_SK_WS_TASK_STACK, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ESPOS_SK_NOTIFICATIONS
    /* Last, not first: registering replays every condition recorded so far,
     * and the replay publishes deltas — which needs the config loaded and the
     * delta engine behind s.lock. A condition raised before SignalK came up
     * (espos_voice starts its wake engine early) reaches the server here. */
    s_notify_ready = true;
    esp_err_t herr = espos_health_add_sink(health_sink, NULL);
    if (herr != ESP_OK && herr != ESP_ERR_INVALID_STATE) {
        /* Not fatal: the stream still runs, conditions just do not become
         * notifications. Loud, because that is a silent loss otherwise. */
        ESP_LOGE(TAG, "could not register the health sink: %s", esp_err_to_name(herr));
        s_notify_ready = false;
    }
#endif
    return ESP_OK;
}

void espos_sk_ws_stop(void)
{
#if CONFIG_ESPOS_SK_NOTIFICATIONS
    /* Before the task goes: a sink left registered would publish into a
     * stopped stream, and a restart would try to register it twice. */
    s_notify_ready = false;
    espos_health_remove_sink(health_sink, NULL);
#endif
    s.stop = true;
    for (int i = 0; i < 100 && s.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
