/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The gateway: signalk-server's BLE provider protocol.
 *
 * Two channels, both bearing the token espos_sk already holds:
 *   POST .../ble/gateway/advertisements   batched advertisements
 *   WS   .../ble/gateway/ws               hello/status out, gatt_* in
 *
 * Both are addressed through espos_sk_http.h, so the scheme follows the
 * selected server's tls flag (http/ws, or https/wss) and the token travels
 * as an Authorization header -- on the WebSocket upgrade too: signalk-server's
 * gateway upgrade handler accepts it from the header, the query string or a
 * cookie, and a header keeps the token out of URL logs.
 *
 * Wire-format invariants (contract with signalk-server's ble-schemas.ts):
 *   - snake_case keys throughout;
 *   - advertisement adv_data is UPPERCASE hex, GATT data is lowercase;
 *   - an empty token means the Authorization header is omitted entirely,
 *     which is valid when the server runs without security.
 */

#include "sdkconfig.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED)

#include <stdatomic.h>
#include <string.h>

#include "ble_gattc.h"
#include "ble_proto.h"
#include "ble_types.h"
#include "cJSON.h"
#if CONFIG_ESPOS_SK_TLS
#include "espos_sk_tls_policy.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "espos_ble.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_net.h"
#include "espos_cfg_keys.h"
#include "espos_sk.h"
#include "espos_sk_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "espos_ble";

/* Defined with the scan-suspend pair further down; declared here because
 * espos_ble_start() subscribes it before that point. */
static void on_portal_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void net_hold(bool down);
static void ota_hold(bool busy);
static SemaphoreHandle_t scan_mutex(void);
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data);

/* espos_ota is optional (a gateway can be updated over USB only), so a weak
 * reference: NULL when it is not linked. */
extern bool espos_ota_busy(void) __attribute__((weak));

#define ADV_PATH "/signalk/v2/api/ble/gateway/advertisements"
#define WS_PATH  "/signalk/v2/api/ble/gateway/ws"

/* Advertisements per POST. Bounded so one flush cannot build an unbounded
 * JSON document out of a busy marina. */
#define POST_BATCH_MAX CONFIG_ESPOS_BLE_POST_BATCH_MAX

typedef struct {
    bool active;
    char session_id[40];
    int conn_handle;
    /* Init writes run in order, each starting when the previous completes. */
    espos_ble_init_write_t *init_writes;
    size_t init_count, init_index;
    int64_t init_deadline_us;
    /* Characteristics to enable notifications on. */
    char (*notify_uuids)[ESPOS_BLE_UUID_MAX];
    size_t notify_count;
} session_t;

#define MAX_SESSIONS ESPOS_BLE_GATTC_MAX_CONN

static struct {
    bool running;
    espos_ble_advq_t q;
    espos_ble_adv_t *storage;
    SemaphoreHandle_t lock;      /* guards q only */
    /* Sessions are touched from three tasks - the websocket event task
     * (gatt_subscribe/write/close), the Bluetooth stack task (every on_gatt_*
     * callback) and the gateway task (the init-write watchdog) - so every
     * lookup, mutation and free goes through this. Without it a session can be
     * freed on one task while another walks it. */
    SemaphoreHandle_t sess_lock;
    esp_websocket_client_handle_t ws;
    bool ws_connected;
    TaskHandle_t task;
    session_t sessions[MAX_SESSIONS];

    /* config */
    bool active_scan, control_ws;
    uint32_t scan_int, scan_win, post_int, status_int, max_gatt;

    /* Set by gateway_task just before it deletes itself, so stop() can wait
     * without polling a task handle that may already be freed. */
    volatile bool task_exited;

    /* How many callers are currently holding the radio away from the
     * scanner. A count, not a flag: a second holder (BLE provisioning)
     * both suspend, and on an unconfigured device they overlap exactly. With
     * a bool, that holder failing to start resumed scanning while the portal
     * -- which had suspended first, and still needed the airtime -- was left
     * believing it had been granted it. Observed on the bench: "scanning
     * suspended (setup portal)" followed 150 ms later by "scanning resumed".
     *
     * Distinct from "not scanning" so the status document can say which it
     * is: on a device showing its portal, a stopped scanner is expected. */
    unsigned scan_suspend_count;

    /* True while the setup portal holds ONE of those suspensions; see
     * espos_ble_portal_hold(). Without it the portal's two announcements --
     * the event and the startup catch-up -- could each take a hold, and the
     * single PORTAL_DOWN would leave the count stuck above zero with the
     * scanner off for good. */
    bool portal_holds_scan;

    /* The same, for "the station has no network": one hold however often
     * NETWORK_DOWN and the startup check report it; see net_hold(). */
    bool net_holds_scan;

    /* And for "an update is downloading". Only gateway_task reads or writes
     * it; see ota_hold(). */
    bool ota_holds_scan;

    /* counters */
    uint32_t adv_received, adv_posted, post_ok, post_fail;
    /* Advertisements dropped because the ingest callback could not take
     * g.lock. Written on the Bluetooth stack task without the lock, read on
     * the API task, so it is atomic rather than a plain counter. */
    atomic_uint_least32_t lock_drops;
} g;

/* ---------------------------------------------------------------- */
/* Advertisement intake (runs on the BT stack task)                   */
/* ---------------------------------------------------------------- */

static void on_advertisement(const espos_ble_adv_t *adv, void *arg)
{
    (void)arg;
    g.adv_received++;
    /* Try-lock, not a timeout: this runs on the Bluetooth stack task, and any
     * wait there stalls the radio -- advertisements the controller discards
     * while it is stalled never reach a counter, so the cost is invisible.
     * Every holder of this lock pushes or swaps and gives it straight back,
     * so a collision costs at most the one advertisement we drop here. */
    if (xSemaphoreTake(g.lock, 0) != pdTRUE) {
        /* Its own counter, not q->dropped: that one is a read-modify-write
         * every other writer performs under g.lock, and incrementing it from
         * here -- the one path that by definition does not hold the lock --
         * would race the ring's own eviction counting. Summed into
         * adv_dropped at read time so the reported tally stays whole. */
        atomic_fetch_add_explicit(&g.lock_drops, 1, memory_order_relaxed);
        return;
    }
    espos_ble_advq_push(&g.q, adv);
    xSemaphoreGive(g.lock);
}

/* esp_get_free_heap_size() asks for MALLOC_CAP_INTERNAL without
 * MALLOC_CAP_8BIT, so it counts the 32-bit-access-only IRAM region that no
 * byte buffer can ever come from -- on an ESP32 that is ~31 kB of heap the
 * server is told about and nothing can use. Report what an allocation would
 * actually find. Credit: SensESP/BLE-gateway, which measured 50776 reported
 * against 19016 real on a HALMET.
 */
static uint32_t free_heap_bytes(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

/* ---------------------------------------------------------------- */
/* HTTP POST of pending advertisements                                */
/* ---------------------------------------------------------------- */

static char *build_adv_body(const espos_ble_adv_t *ads, size_t n)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "gateway_id", espos_ble_mac());
    cJSON_AddStringToObject(root, "mac", espos_ble_mac());
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", (double)free_heap_bytes());

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (size_t i = 0; i < n && devs; i++) {
        cJSON *d = cJSON_CreateObject();
        if (!d) break;
        cJSON_AddStringToObject(d, "mac", ads[i].address);
        cJSON_AddNumberToObject(d, "rssi", ads[i].rssi);
        if (ads[i].name[0]) cJSON_AddStringToObject(d, "name", ads[i].name);
        if (ads[i].adv_data_len) {
            char hex[ESPOS_BLE_ADV_DATA_MAX * 2 + 1];
            /* UPPERCASE here; the GATT path uses lowercase. */
            espos_ble_hex_encode_upper(ads[i].adv_data, ads[i].adv_data_len, hex);
            cJSON_AddStringToObject(d, "adv_data", hex);
        }
        cJSON_AddItemToArray(devs, d);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Scratch for one POST. Deliberately static rather than stack: the batch
 * alone is POST_BATCH_MAX * sizeof(espos_ble_adv_t) (~5 KB), which overflows
 * the gateway task's stack the first time a POST actually runs - and that
 * only happens once a token exists, so it hides until the device is fully
 * provisioned. Only the gateway task touches these. */
static espos_ble_adv_t s_post_batch[POST_BATCH_MAX];

static void post_pending(void)
{
    /* Checked here, not left to the helper: a drained batch with nowhere to
     * post would be lost, so the ring keeps buffering until a server exists. */
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK || !srv.host[0]) return;

    espos_ble_adv_t *batch = s_post_batch;
    size_t n = 0;
    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = espos_ble_advq_drain(&g.q, batch, POST_BATCH_MAX);
        xSemaphoreGive(g.lock);
    }
    if (!n) return;

    char *body = build_adv_body(batch, n);
    if (!body) return;

    /* espos_sk owns the client: scheme from the server's tls flag, the token
     * as a Bearer header (no header at all when it is empty - correct when
     * the server has security off), a fresh handle per call, and a 401/403
     * reported to the token machine instead of answered with an access
     * request of our own. The reply body is a status line at most. */
    espos_sk_http_opts_t opts = { .timeout_ms = 3000, .max_body = 512 };
    espos_sk_http_resp_t r;
    esp_err_t err = espos_sk_http_post(ADV_PATH, body, &opts, &r);
    free(body);

    if (err == ESP_OK && r.status == 200) {
        g.adv_posted += n;
        g.post_ok++;
    } else {
        g.post_fail++;
        if (r.status == 401 || r.status == 403) {
            ESP_LOGW(TAG, "POST rejected (%d) - espos_sk is re-checking the token", r.status);
        } else {
            ESP_LOGW(TAG, "POST failed: err=%s status=%d", esp_err_to_name(err), r.status);
        }
    }
    espos_sk_http_resp_free(&r);
}

/* ---------------------------------------------------------------- */
/* Control WebSocket                                                  */
/* ---------------------------------------------------------------- */

static void ws_send_json(cJSON *doc)
{
    if (!g.ws_connected || !g.ws) return;
    char *txt = cJSON_PrintUnformatted(doc);
    if (!txt) return;
    /* Short, because run_subscribes()/run_init_writes() report through here
     * while holding the session lock, on the Bluetooth stack task. A long
     * timeout there stalls the radio; a dropped status frame does not. */
    esp_websocket_client_send_text(g.ws, txt, strlen(txt), pdMS_TO_TICKS(100));
    free(txt);
}

static void send_hello(void)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", "hello");
    cJSON_AddStringToObject(d, "gateway_id", espos_ble_mac());
    cJSON_AddStringToObject(d, "mac", espos_ble_mac());
    cJSON_AddStringToObject(d, "firmware", "espos-ble-gateway");
    cJSON_AddNumberToObject(d, "max_gatt_connections", g.max_gatt);
    cJSON_AddNumberToObject(d, "active_gatt_connections",
                            espos_ble_gatt_active_count());
    ws_send_json(d);
    cJSON_Delete(d);
}

static void send_status(void)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", "status");
    cJSON_AddStringToObject(d, "gateway_id", espos_ble_mac());
    cJSON_AddNumberToObject(d, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(d, "free_heap", (double)free_heap_bytes());
    cJSON_AddNumberToObject(d, "scan_hits", espos_ble_scan_hits());
    cJSON_AddNumberToObject(d, "post_success", g.post_ok);
    cJSON_AddNumberToObject(d, "post_fail", g.post_fail);
    cJSON_AddNumberToObject(d, "active_gatt_connections",
                            espos_ble_gatt_active_count());
    cJSON_AddNumberToObject(d, "max_gatt_connections", g.max_gatt);
    ws_send_json(d);
    cJSON_Delete(d);
}

/* Held across the whole of any session operation, including the callbacks it
 * makes into ble_gattc.
 *
 * The mutex is NOT recursive, so nothing called under it may re-enter these
 * helpers. That is why espos_ble_gatt_write() reports a no-response write
 * through its return value instead of invoking on_gatt_write_done() inline:
 * doing so deadlocked the init chain, which then only advanced when the 3 s
 * watchdog fired. */
/* Must exceed the worst case a holder can take. The longest is a session
 * bring-up that reports several events through ws_send_json(), each capped at
 * 100 ms. 500 ms leaves room for that without letting a degraded websocket
 * make every other task fail to acquire the lock.
 *
 * The deeper fix is to hand callback work to gateway_task through a queue so
 * the Bluetooth stack task never blocks at all; that is a larger change than
 * this one is worth, and the timeouts here bound the exposure meanwhile. */
static inline bool sess_lock(void)
{
    return g.sess_lock && xSemaphoreTake(g.sess_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static inline void sess_unlock(void)
{
    if (g.sess_lock) xSemaphoreGive(g.sess_lock);
}

static session_t *session_by_id(const char *id)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g.sessions[i].active && strcmp(g.sessions[i].session_id, id) == 0) {
            return &g.sessions[i];
        }
    }
    return NULL;
}

static session_t *session_by_handle(int h)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (g.sessions[i].active && g.sessions[i].conn_handle == h) {
            return &g.sessions[i];
        }
    }
    return NULL;
}

static void session_free(session_t *s)
{
    free(s->notify_uuids);
    free(s->init_writes);
    memset(s, 0, sizeof(*s));
}

static void send_gatt_event(const char *type, const char *session_id,
                            const char *uuid, const char *data_hex,
                            const char *error, int reason)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "type", type);
    cJSON_AddStringToObject(d, "session_id", session_id);
    if (uuid) cJSON_AddStringToObject(d, "uuid", uuid);
    if (data_hex) cJSON_AddStringToObject(d, "data", data_hex);
    if (error) cJSON_AddStringToObject(d, "error", error);
    if (reason >= 0) cJSON_AddNumberToObject(d, "reason", reason);
    ws_send_json(d);
    cJSON_Delete(d);
}

/* Drive the init-write chain. Each write starts when the previous completes -
 * except a write-without-response, which produces no completion event at all,
 * so ble_gattc synthesises one. The deadline is the backstop: without it a
 * peripheral that simply never answers leaves the session stuck in init
 * forever, never subscribing and never reporting. */
/* Enable notifications before any init write goes out.
 *
 * Order matters: a BMS answers its first command over the notify pipe, so
 * subscribing afterwards races the reply and usually loses it - the write
 * lands, the peripheral answers into a pipe nobody is listening on, and the
 * session sits there looking connected but silent. */
static void run_subscribes(session_t *s)
{
    for (size_t i = 0; i < s->notify_count; i++) {
        esp_err_t err = espos_ble_gatt_subscribe(s->conn_handle, s->notify_uuids[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "subscribe %s: %s", s->notify_uuids[i], esp_err_to_name(err));
            send_gatt_event("gatt_error", s->session_id, s->notify_uuids[i], NULL,
                            "subscribe failed", -1);
        }
    }
}

/* Iterative, not recursive.
 *
 * init[] comes straight off the wire, so its length is chosen by the server;
 * recursing once per write would let a peer pick the stack depth. Writes that
 * expect a completion return here and are resumed by on_gatt_write_done(),
 * which re-enters with a strictly larger init_index - so that path terminates
 * too.
 *
 * Caller holds the session lock. */
static void run_init_writes(session_t *s)
{
    while (s->init_index < s->init_count) {
        espos_ble_init_write_t *iw = &s->init_writes[s->init_index];
        s->init_deadline_us = esp_timer_get_time() + 3000000; /* 3 s */
        esp_err_t err = espos_ble_gatt_write(s->conn_handle, iw->char_uuid,
                                             iw->data, iw->data_len, iw->mode);

        if (err == ESP_OK) {
            /* Sent, and a WRITE_CHAR_EVT is coming: on_gatt_write_done()
             * advances the chain and calls back in. */
            return;
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            /* Write-without-response: no completion event will ever arrive,
             * so advance here and keep going. */
            s->init_index++;
            continue;
        }
        send_gatt_event("gatt_error", s->session_id, iw->char_uuid, NULL,
                        "init write failed", -1);
        s->init_index++;
    }
    send_gatt_event("gatt_connected", s->session_id, NULL, NULL, NULL, -1);
}

static void handle_gatt_subscribe(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(doc, "mac");
    const cJSON *svc = cJSON_GetObjectItemCaseSensitive(doc, "service");
    if (!cJSON_IsString(sid) || !cJSON_IsString(mac)) return;

    if (!sess_lock()) {
        send_gatt_event("gatt_error", sid->valuestring, NULL, NULL,
                        "gateway busy", -1);
        return;
    }

    /* Only look at the slots the operator allows, so max_gatt_sess is enforced
     * rather than merely advertised in hello. */
    size_t limit = g.max_gatt < MAX_SESSIONS ? g.max_gatt : MAX_SESSIONS;
    session_t *s = NULL;
    for (size_t i = 0; i < limit; i++) {
        if (!g.sessions[i].active) {
            s = &g.sessions[i];
            break;
        }
    }
    if (!s) {
        sess_unlock();
        send_gatt_event("gatt_error", sid->valuestring, NULL, NULL,
                        "no free session slot", -1);
        return;
    }

    memset(s, 0, sizeof(*s));
    snprintf(s->session_id, sizeof(s->session_id), "%s", sid->valuestring);

    const cJSON *notify = cJSON_GetObjectItemCaseSensitive(doc, "notify");
    if (cJSON_IsArray(notify)) {
        int n = cJSON_GetArraySize(notify);
        if (n > 0) {
            s->notify_uuids = calloc((size_t)n, ESPOS_BLE_UUID_MAX);
            if (s->notify_uuids) {
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(notify, i);
                    if (!cJSON_IsString(e)) continue;
                    snprintf(s->notify_uuids[s->notify_count], ESPOS_BLE_UUID_MAX,
                             "%s", e->valuestring);
                    s->notify_count++;
                }
            }
        }
    }

    const cJSON *init = cJSON_GetObjectItemCaseSensitive(doc, "init");
    if (cJSON_IsArray(init)) {
        int n = cJSON_GetArraySize(init);
        if (n > 0) {
            s->init_writes = calloc((size_t)n, sizeof(*s->init_writes));
            if (s->init_writes) {
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(init, i);
                    const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "uuid");
                    const cJSON *d = cJSON_GetObjectItemCaseSensitive(e, "data");
                    if (!cJSON_IsString(u) || !cJSON_IsString(d)) continue;
                    espos_ble_init_write_t *iw = &s->init_writes[s->init_count];
                    snprintf(iw->char_uuid, sizeof(iw->char_uuid), "%s", u->valuestring);
                    int len = espos_ble_hex_decode(d->valuestring, iw->data,
                                                   sizeof(iw->data));
                    if (len < 0) continue;
                    iw->data_len = (size_t)len;
                    /* Absent means write-with-response; see ble_proto. */
                    iw->mode = espos_ble_parse_write_mode(e);
                    s->init_count++;
                }
            }
        }
    }

    /* Active BEFORE the connect: discovery completes on the Bluetooth stack
     * task and can call back before this function returns. Setting the flag
     * afterwards loses that race - session_by_handle() skips the slot, the
     * subscribes and init writes never run, and the peripheral sits connected
     * and silent with nothing reported to the server. */
    s->active = true;
    s->conn_handle = espos_ble_gatt_connect(
        mac->valuestring, cJSON_IsString(svc) ? svc->valuestring : NULL);
    if (s->conn_handle < 0) {
        char sid_copy[sizeof(s->session_id)];
        snprintf(sid_copy, sizeof(sid_copy), "%s", s->session_id);
        session_free(s);
        sess_unlock();
        send_gatt_event("gatt_error", sid_copy, NULL, NULL, "connect failed", -1);
        return;
    }
    sess_unlock();
}

static void handle_gatt_write(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    const cJSON *uuid = cJSON_GetObjectItemCaseSensitive(doc, "uuid");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(doc, "data");
    if (!cJSON_IsString(sid) || !cJSON_IsString(uuid) || !cJSON_IsString(data)) return;

    if (!sess_lock()) return;
    session_t *s = session_by_id(sid->valuestring);
    if (!s) {
        sess_unlock();
        return;
    }

    uint8_t buf[ESPOS_BLE_WRITE_DATA_MAX];
    int len = espos_ble_hex_decode(data->valuestring, buf, sizeof(buf));
    if (len < 0) {
        char sid_copy[sizeof(s->session_id)];
        snprintf(sid_copy, sizeof(sid_copy), "%s", s->session_id);
        sess_unlock();
        send_gatt_event("gatt_error", sid_copy, uuid->valuestring, NULL,
                        "bad hex payload", -1);
        return;
    }
    espos_ble_gatt_write(s->conn_handle, uuid->valuestring, buf, (size_t)len,
                         espos_ble_parse_write_mode(doc));
    sess_unlock();
}

static void handle_gatt_close(cJSON *doc)
{
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(doc, "session_id");
    if (!cJSON_IsString(sid)) return;
    if (!sess_lock()) return;
    session_t *s = session_by_id(sid->valuestring);
    if (!s) {
        sess_unlock();
        return;
    }
    espos_ble_gatt_disconnect(s->conn_handle);
    session_free(s);
    sess_unlock();
}

static void handle_ws_text(const char *data, size_t len)
{
    cJSON *doc = cJSON_ParseWithLength(data, len);
    if (!doc) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(doc, "type");
    if (cJSON_IsString(type)) {
        if (!strcmp(type->valuestring, "gatt_subscribe"))
            handle_gatt_subscribe(doc);
        else if (!strcmp(type->valuestring, "gatt_write"))
            handle_gatt_write(doc);
        else if (!strcmp(type->valuestring, "gatt_close"))
            handle_gatt_close(doc);
        /* hello_ack needs no action beyond confirming the server heard us. */
    }
    cJSON_Delete(doc);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        g.ws_connected = true;
        ESP_LOGI(TAG, "control WS connected");
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        g.ws_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        /* op_code 1 is a text frame; ignore pings/pongs/binary. */
        if (ev->op_code == 0x01 && ev->data_len > 0) {
            handle_ws_text(ev->data_ptr, ev->data_len);
        }
        break;
    default:
        break;
    }
}

/* The server the live control socket was opened against - host, port and
 * scheme - so a change is noticed rather than silently ignored for the
 * lifetime of the handle. */
static char s_ws_host[ESPOS_SK_HOST_MAX];
static uint16_t s_ws_port;
static bool s_ws_tls;

/* Upgrade headers and the token they are built from. Static, like the POST
 * scratch: 2 KB is too much for the gateway task's stack, and only that task
 * runs here. esp_websocket_client_init() copies the headers. */
static char s_ws_token[ESPOS_SK_TOKEN_MAX];
static char s_ws_headers[ESPOS_SK_TOKEN_MAX + 32];

static void ws_connect(void)
{
    if (!g.control_ws) return;
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK || !srv.host[0]) return;

    if (g.ws) {
        /* Already connected to this server: nothing to do. Pointed at a
         * different one (the user re-pinned it, discovery moved, or sk.tls
         * flipped): tear the old socket down so the code below re-dials. */
        if (strcmp(s_ws_host, srv.host) == 0 && s_ws_port == srv.port && s_ws_tls == srv.tls) return;
        ESP_LOGI(TAG, "control WS server changed %s://%s:%u -> %s://%s:%u",
                 s_ws_tls ? "wss" : "ws", s_ws_host, s_ws_port,
                 srv.tls ? "wss" : "ws", srv.host, srv.port);
        esp_websocket_client_stop(g.ws);
        esp_websocket_client_destroy(g.ws);
        g.ws = NULL;
        g.ws_connected = false;
    }

    char url[ESPOS_SK_URL_MAX];
    if (espos_sk_ws_url(WS_PATH, url, sizeof(url)) != ESP_OK) return;

    /* The token travels as an upgrade header, as on the delta stream;
     * signalk-server's gateway handler reads it from the header, the query
     * string or a cookie. No token, no header: correct against an open server. */
    s_ws_token[0] = '\0';
    espos_sk_get_token(s_ws_token, sizeof(s_ws_token));
    if (s_ws_token[0]) {
        snprintf(s_ws_headers, sizeof(s_ws_headers), "Authorization: Bearer %s\r\n", s_ws_token);
    } else {
        s_ws_headers[0] = '\0';
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .headers = s_ws_headers[0] ? s_ws_headers : NULL,
        .task_stack = 6144,
        /* Roomy enough that a large gatt_subscribe arrives in one frame; the
         * implementation this replaces used 1 KB and could not reassemble. */
        .buffer_size = CONFIG_ESPOS_BLE_WS_BUFFER,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
#if CONFIG_ESPOS_SK_TLS
        /* wss trusts exactly what the delta stream trusts: the shared hook
         * carries the pinned anchor, so the boat's own self-signed server is
         * accepted here too instead of on every socket but this one. */
        .crt_bundle_attach = srv.tls ? espos_sk_tls_attach : NULL,
        .skip_cert_common_name_check = srv.tls && espos_sk_tls_trust_mode() != ESPOS_SK_TLS_TRUST_BUNDLE,
#endif
    };
    g.ws = esp_websocket_client_init(&cfg);
    if (!g.ws) return;
    esp_websocket_register_events(g.ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(g.ws);
    snprintf(s_ws_host, sizeof(s_ws_host), "%s", srv.host);
    s_ws_port = srv.port;
    s_ws_tls = srv.tls;
}

/* ---------------------------------------------------------------- */
/* GATT callbacks                                                     */
/* ---------------------------------------------------------------- */

static void on_gatt_connected(int h, void *arg)
{
    (void)arg;
    if (!sess_lock()) return;
    session_t *s = session_by_handle(h);
    if (!s) {
        sess_unlock();
        return;
    }
    run_subscribes(s);
    run_init_writes(s);
    sess_unlock();
}

static void on_gatt_disconnected(int h, int reason, void *arg)
{
    (void)arg;
    if (!sess_lock()) return;
    session_t *s = session_by_handle(h);
    if (!s) {
        sess_unlock();
        return;
    }
    char sid[sizeof(s->session_id)];
    snprintf(sid, sizeof(sid), "%s", s->session_id);
    session_free(s);
    sess_unlock();
    /* Report outside the lock: the send can block on the websocket. */
    send_gatt_event("gatt_disconnected", sid, NULL, NULL, NULL, reason);
}

static void on_gatt_notify(int h, const char *uuid, const uint8_t *data,
                           size_t len, void *arg)
{
    (void)arg;
    char sid[sizeof(g.sessions[0].session_id)];
    if (!sess_lock()) return;
    session_t *s = session_by_handle(h);
    if (!s) {
        sess_unlock();
        return;
    }
    snprintf(sid, sizeof(sid), "%s", s->session_id);
    sess_unlock();

    /* len comes from the peripheral (bounded by the negotiated MTU), so cap it
     * rather than sizing an allocation from a remote value. A GATT payload
     * cannot exceed the ATT maximum. */
    if (len > 512) {
        ESP_LOGW(TAG, "notify %u bytes truncated to 512", (unsigned)len);
        len = 512;
    }
    char *hex = malloc(len * 2 + 1);
    if (!hex) return;
    /* lowercase on the GATT channel */
    espos_ble_hex_encode_lower(data, len, hex);
    send_gatt_event("gatt_data", sid, uuid, hex, NULL, -1);
    free(hex);
}

static void on_gatt_read(int h, const char *uuid, const uint8_t *data,
                         size_t len, void *arg)
{
    on_gatt_notify(h, uuid, data, len, arg);
}

static void on_gatt_write_done(int h, const char *uuid, bool ok, void *arg)
{
    (void)arg;
    if (!sess_lock()) return;
    session_t *s = session_by_handle(h);
    if (!s || s->init_index >= s->init_count) {
        sess_unlock();
        return;
    }

    /* Only a completion for the write we are actually waiting on advances the
     * chain. A server-driven gatt_write during initialisation completes on the
     * same callback, and taking it would skip one configured init write and
     * report gatt_connected early.
     *
     * Compare by value, not text: the callback always formats the long
     * lowercase form while the server may have sent "ffe1" or upper case, so
     * strcmp() would never match and the chain would stall until the watchdog
     * fired. */
    if (uuid) {
        uint8_t got[16], want[16];
        if (espos_ble_uuid_parse(uuid, got, NULL) &&
            espos_ble_uuid_parse(s->init_writes[s->init_index].char_uuid, want, NULL) &&
            !espos_ble_uuid_equal(got, want)) {
            sess_unlock();
            return;
        }
    }

    if (!ok) ESP_LOGW(TAG, "init write %u failed", (unsigned)s->init_index);
    s->init_index++;
    run_init_writes(s);
    sess_unlock();
}

static void on_gatt_error(int h, const char *error, void *arg)
{
    (void)arg;
    if (!sess_lock()) return;
    session_t *s = session_by_handle(h);
    if (!s) {
        sess_unlock();
        return;
    }
    char sid[sizeof(s->session_id)];
    snprintf(sid, sizeof(sid), "%s", s->session_id);
    session_free(s);
    sess_unlock();
    send_gatt_event("gatt_error", sid, NULL, NULL, error, -1);
}

/* ---------------------------------------------------------------- */
/* Task                                                               */
/* ---------------------------------------------------------------- */

static void gateway_task(void *arg)
{
    (void)arg;
    int64_t next_post = 0, next_status = 0;

    while (g.running) {
        /* First, ahead of the POST below: that can take seconds, and an
         * update starting meanwhile should get the radio back as soon as
         * this loop can notice. */
        ota_hold(espos_ota_busy && espos_ota_busy());

        int64_t now = esp_timer_get_time();
        espos_sk_ws_status_t ws;
        bool sk_up = (espos_sk_ws_get_status(&ws) == ESP_OK) && ws.connected;

        /* Gate on the SignalK link: without it there is nowhere to post and
         * no token worth using. Advertisements keep buffering meanwhile. */
        if (sk_up) {
            if (now >= next_post) {
                post_pending();
                next_post = now + (int64_t)g.post_int * 1000;
            }
            if (g.control_ws) {
                ws_connect();
                if (now >= next_status) {
                    send_status();
                    next_status = now + (int64_t)g.status_int * 1000;
                }
            }
        } else if (g.ws) {
            /* SignalK went away: drop the control socket too, rather than
             * leaving a handle pointed at a server that is gone. Without this
             * the client keeps retrying an address the device has since been
             * moved off, and /ble/status keeps reporting ws_connected from a
             * stale handle. It is re-dialled from scratch above once the SK
             * link is back, so a server change is picked up. */
            ESP_LOGI(TAG, "SignalK link down - closing the control WS");
            esp_websocket_client_stop(g.ws);
            esp_websocket_client_destroy(g.ws);
            g.ws = NULL;
            g.ws_connected = false;
        }

        /* Init-write watchdog: a peripheral that never answers must not pin a
         * session in init forever. */
        if (sess_lock()) {
            for (size_t i = 0; i < MAX_SESSIONS; i++) {
                session_t *s = &g.sessions[i];
                if (!s->active || s->init_index >= s->init_count) continue;
                if (s->init_deadline_us && now > s->init_deadline_us) {
                    ESP_LOGW(TAG, "init write %u timed out",
                             (unsigned)s->init_index);
                    s->init_index++;
                    run_init_writes(s);
                }
            }
            sess_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ota_hold(false);
    g.task_exited = true;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- */
/* Public API                                                         */
/* ---------------------------------------------------------------- */

esp_err_t espos_ble_reserve_controller(void)
{
    /* Honour the same switch espos_ble_start() honours. A device with BLE
     * turned off must not have 24 KB taken from it for a radio it will never
     * use -- and on a part this tight that is the difference between the rest
     * of the firmware fitting and not. */
    bool enabled = true;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_ENABLED, &enabled);
    if (!enabled) {
        return ESP_OK;
    }

    /* The controller only. Not espos_ble_backend_init(), which also brings up
     * the Bluedroid host: that starts the BTU and BTC threads, whose stacks are
     * internal RAM, so it would spend exactly the memory this call exists to
     * protect. The host comes up in espos_ble_start() with the callbacks. */
    return espos_ble_backend_controller_only();
}

esp_err_t espos_ble_start(void)
{
    if (g.running) return ESP_OK;

    bool enabled = true;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_ENABLED, &enabled);
    if (!enabled) {
        ESP_LOGI(TAG, "disabled by config");
        return ESP_OK;
    }

    int32_t v;
    g.active_scan = false;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_ACTIVE_SCAN, &g.active_scan);
    v = 320;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_SCAN_INT_MS, &v);
    g.scan_int = (uint32_t)v;
    v = 160;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_SCAN_WIN_MS, &v);
    g.scan_win = (uint32_t)v;
    v = 2000;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_POST_INT_MS, &v);
    g.post_int = (uint32_t)v;
    v = 30000;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_STATUS_INT_MS, &v);
    g.status_int = (uint32_t)v;
    v = 3;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_MAX_GATT_SESS, &v);
    if (v < 0) v = 0;
    if (v > MAX_SESSIONS) v = MAX_SESSIONS;
    g.max_gatt = (uint32_t)v;
    g.control_ws = true;
    espos_config_get_bool(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_CONTROL_WS, &g.control_ws);

    int32_t cap = 500;
    espos_config_get_i32(ESPOS_CFG_NS_BLE, ESPOS_CFG_BLE_MAX_PEND_ADS, &cap);
    /* The descriptor bounds this, but config can be restored from an older
     * schema or a hand-edited import: 0 would give a zero-capacity ring that
     * drops everything, and a negative would become a huge size_t. */
    if (cap < 10) cap = 10;
    if (cap > 5000) cap = 5000;

    /* Do not ask for more than the heap can give, and leave the rest of the
     * gateway room to start.
     *
     * The ring is one contiguous block, and espos_ble_adv_t is 128 B once
     * padded, so the default 500 entries is 62.5 KB. On an ESP32-C5 -- single
     * core, ~166 KiB of internal RAM, running WiFi in STA+softAP with an HTTP
     * server -- the free heap at this point is 25-27 KB, so the allocation
     * could never succeed and BLE never started at all. The ESP_ERR_NO_MEM it
     * returned was indistinguishable from the radio itself being short, which
     * is what made it look like a Bluedroid problem (espOS #124).
     *
     * A quarter of the largest free block is the budget: enough that the ring
     * is useful, little enough that the controller, its tasks and the scan
     * callbacks still have somewhere to live. Shrinking beats failing -- the
     * ring's whole contract is that a full ring drops the oldest and counts
     * them, so a smaller one is a degraded gateway rather than no gateway. */
    /* Prefer external RAM where the board has it. The ring is a plain data
     * buffer -- advertisements are written by the scan callback and read by the
     * POST task, both on the CPU, and nothing DMAs into it -- so it does not
     * belong in the internal/DMA pool that the radio, lwIP and every task stack
     * compete for. Measured on an ESP32-C5 with 8 MB of PSRAM (espOS #127):
     * espos_start() consumed 160 KB of DMA-capable memory, BLE 51 KB of it, and
     * the device rebooted on lowMemory with 7 KB left while 8.2 MB of PSRAM sat
     * untouched.
     *
     * MALLOC_CAP_SPIRAM first, then plain 8BIT: heap_caps_malloc() with SPIRAM
     * returns NULL rather than falling back, and most espOS targets have no
     * external RAM at all. */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    bool ring_external = largest > 0;
    if (!ring_external) {
        largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    }
    /* A quarter of the block when the ring competes with the radio and every
     * task stack for internal RAM. In external RAM it competes with nothing --
     * 8 MB against a 62 KB default -- so the clamp is only a backstop there and
     * a quarter would refuse the configured size for no reason. */
    size_t budget = ring_external ? largest : largest / 4;
    int32_t affordable = (int32_t)(budget / sizeof(espos_ble_adv_t));
    if (affordable < 10) affordable = 10;   /* the descriptor's own floor */
    if (cap > affordable) {
        ESP_LOGW(TAG, "advertisement buffer %u entries needs %u B; largest free "
                      "%s block is %u B, using %u entries",
                 (unsigned)cap, (unsigned)((size_t)cap * sizeof(espos_ble_adv_t)),
                 ring_external ? "external" : "internal", (unsigned)largest,
                 (unsigned)affordable);
        cap = affordable;
    }

    g.storage = ring_external
                    ? heap_caps_calloc((size_t)cap, sizeof(espos_ble_adv_t), MALLOC_CAP_SPIRAM)
                    : calloc((size_t)cap, sizeof(espos_ble_adv_t));
    if (!g.storage && ring_external) {
        /* PSRAM was there a moment ago and would not serve the request. Fall back
         * rather than refuse to start: a gateway with a small internal ring beats
         * no gateway.
         *
         * Re-clamp first. `cap` above was sized against the EXTERNAL block --
         * megabytes -- and asking internal RAM for that would either fail outright
         * or, worse, succeed and take the memory the radio needs. So redo the
         * quarter-of-the-largest-block arithmetic against the pool actually being
         * used. */
        ring_external = false;
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int32_t internal_affordable = (int32_t)((internal_largest / 4) / sizeof(espos_ble_adv_t));
        if (internal_affordable < 10) internal_affordable = 10;
        ESP_LOGW(TAG, "advertisement buffer: PSRAM refused %u B; internal largest "
                      "free %u B, using %u entries",
                 (unsigned)((size_t)cap * sizeof(espos_ble_adv_t)), (unsigned)internal_largest,
                 (unsigned)internal_affordable);
        if (cap > internal_affordable) {
            cap = internal_affordable;
        }
        /* Explicit caps rather than calloc(): on a PSRAM board plain calloc() is
         * subject to the malloc policy and could hand back external memory again,
         * which is the allocation that just failed. */
        g.storage = heap_caps_calloc((size_t)cap, sizeof(espos_ble_adv_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!g.storage) {
        ESP_LOGE(TAG, "no memory for %u advertisement slots (%u B)",
                 (unsigned)cap, (unsigned)((size_t)cap * sizeof(espos_ble_adv_t)));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "advertisement buffer %u entries (%u B) in %s RAM", (unsigned)cap,
             (unsigned)((size_t)cap * sizeof(espos_ble_adv_t)),
             ring_external ? "external" : "internal");
    espos_ble_advq_init(&g.q, g.storage, (size_t)cap);

    g.lock = xSemaphoreCreateMutex();
    g.sess_lock = xSemaphoreCreateMutex();
    if (!g.lock || !g.sess_lock) {
        if (g.lock) {
            vSemaphoreDelete(g.lock);
            g.lock = NULL;
        }
        if (g.sess_lock) {
            vSemaphoreDelete(g.sess_lock);
            g.sess_lock = NULL;
        }
        free(g.storage);
        g.storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    espos_ble_callbacks_t cb = {
        .on_advertisement = on_advertisement,
        .on_gatt_connected = on_gatt_connected,
        .on_gatt_disconnected = on_gatt_disconnected,
        .on_gatt_notify = on_gatt_notify,
        .on_gatt_read = on_gatt_read,
        .on_gatt_write_done = on_gatt_write_done,
        .on_gatt_error = on_gatt_error,
    };
    esp_err_t err = espos_ble_backend_init(&cb);
    if (err != ESP_OK) {
        vSemaphoreDelete(g.lock);
        vSemaphoreDelete(g.sess_lock);
        g.lock = g.sess_lock = NULL;
        free(g.storage);
        g.storage = NULL;
        return err;
    }

    /* Not if something already asked for the radio. espos_core suspends the
     * scanner before this when the setup portal is up, and starting anyway
     * would hand back the airtime it just took -- the portal is raised inside
     * espos_wifi_start(), which runs first, so this ordering is the normal
     * case on an unconfigured device rather than a corner. */
    /* Before the scan decision, so a device still associating never arms the
     * scanner only to suspend it a moment later. Subscribed first and checked
     * second: an UP that lands in between is then either heard or already
     * visible to espos_net_is_up(), never lost. */
    if (espos_event_subscribe(ESPOS_EVENT_NETWORK_UP, on_net_event, NULL) == ESP_OK &&
        espos_event_subscribe(ESPOS_EVENT_NETWORK_DOWN, on_net_event, NULL) == ESP_OK) {
        net_hold(!espos_net_is_up());
    } else {
        espos_event_unsubscribe(ESPOS_EVENT_NETWORK_UP, on_net_event);
        ESP_LOGW(TAG, "not watching the network; scanning while it is down");
    }

    /* Running and the scan decision change together under the scan mutex: a
     * hold released between them (NETWORK_UP arriving now) would otherwise see
     * a gateway not yet running, start nothing, and leave the scanner off. */
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    g.running = true;
    if (g.scan_suspend_count == 0) {
        espos_ble_scan_start(g.active_scan, (uint16_t)g.scan_int, (uint16_t)g.scan_win);
    } else {
        ESP_LOGI(TAG, "not scanning yet: %s", "suspended before the gateway started");
    }
    xSemaphoreGiveRecursive(scan_mutex());

    g.task_exited = false;
    /* 8 KB by default: cJSON serialisation plus the HTTP helper's own frame
     * need real headroom, and a stack-protection fault here is a reboot loop
     * rather than a degraded mode. The big scratch buffers are static (see
     * s_post_batch) so this covers the call depth, not the payloads. */
    if (xTaskCreate(gateway_task, "espos_ble", CONFIG_ESPOS_BLE_STACK_SIZE, NULL, 5, &g.task) != pdPASS) {
        g.running = false;
        espos_ble_scan_stop();
        vSemaphoreDelete(g.lock);
        vSemaphoreDelete(g.sess_lock);
        g.lock = g.sess_lock = NULL;
        free(g.storage);
        g.storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Stand down while the setup portal is up; see on_portal_event. Also
     * non-fatal: without it the gateway still scans, it just makes the portal
     * painfully slow to join on a shared-radio part. */
    esp_err_t ev_err = espos_event_subscribe(ESPOS_EVENT_PORTAL_UP, on_portal_event, NULL);
    if (ev_err == ESP_OK) {
        ev_err = espos_event_subscribe(ESPOS_EVENT_PORTAL_DOWN, on_portal_event, NULL);
        if (ev_err != ESP_OK) {
            /* Half a subscription is worse than none: we would suspend on
             * PORTAL_UP and never hear the DOWN that resumes us, leaving a
             * gateway that stops scanning the first time the portal appears
             * and never starts again. */
            espos_event_unsubscribe(ESPOS_EVENT_PORTAL_UP, on_portal_event);
        }
    }
    if (ev_err != ESP_OK) {
        ESP_LOGW(TAG, "not watching the portal: %s; joining it may be slow", esp_err_to_name(ev_err));
    }
    /* Non-fatal: the gateway still works without its status endpoint. */
    esp_err_t api_err = espos_ble_register_api();
    if (api_err != ESP_OK) {
        ESP_LOGW(TAG, "status endpoint unavailable: %s", esp_err_to_name(api_err));
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

/* The setup portal is up, so the radio belongs to WiFi for a while.
 *
 * On the ESP32-P4 the C6 co-processor is ONE radio doing both, and the
 * gateway's default scan is 160 ms of listening every 320 ms -- half the
 * airtime. That is enough to lose the SoftAP's beacons and the association
 * and DHCP exchange with them: joining the portal takes minutes, or never
 * completes, which reads to whoever is holding the phone as a broken device.
 *
 * A device showing its setup portal has, by definition, nowhere to publish
 * advertisements to yet, so there is nothing to lose by standing down. */
/* The count is read-modify-written by every holder, and holders are not all
 * on one task: the portal arrives on the default event loop's task, the
 * startup catch-up on whichever task called espos_start(), and a firmware
 * with its own holder may use a third. A spinlock rather than the component's
 * mutex because the critical section is three instructions and one of these
 * callers may be an event handler that must not block. */
static portMUX_TYPE s_suspend_lock = portMUX_INITIALIZER_UNLOCKED;

/* Orders whole hold transitions -- flag, count and the scanner call -- where
 * the spinlock above only guards the count. Without it a DOWN that has set
 * its flag but not yet suspended can lose to an UP on another task: the UP
 * resumes nothing, the DOWN then suspends, and the scanner stays off with the
 * network up. Recursive because a hold takes it and then calls
 * espos_ble_scan_suspend(), which takes it again. Every caller is a task (an
 * event handler, espos_start(), provisioning), so blocking here is allowed.
 * Created on first use: espos_core can take the portal hold before
 * espos_ble_start() runs. */
static StaticSemaphore_t s_scan_mutex_buf;
static SemaphoreHandle_t s_scan_mutex;

static SemaphoreHandle_t scan_mutex(void)
{
    taskENTER_CRITICAL(&s_suspend_lock);
    if (s_scan_mutex == NULL) {
        s_scan_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_scan_mutex_buf);
    }
    taskEXIT_CRITICAL(&s_suspend_lock);
    return s_scan_mutex;
}

/* The portal is ONE holder of the scan suspension, however many times we are
 * told about it: the event fires for later transitions, and espos_start()
 * checks at boot because the portal is raised inside espos_wifi_start(),
 * before the gateway exists to hear the event. Whichever arrives second must
 * not take a second hold -- only one PORTAL_DOWN ever arrives to release it,
 * and the count would never reach zero, leaving the scanner off for good. */
void espos_ble_portal_hold(bool up)
{
    /* Test-and-set under the same lock as the count: the event and the
     * startup catch-up can land on different tasks, and two "portal is up"
     * arriving at once must still take exactly one hold. */
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    taskENTER_CRITICAL(&s_suspend_lock);
    bool changed = (up != g.portal_holds_scan);
    g.portal_holds_scan = up;
    taskEXIT_CRITICAL(&s_suspend_lock);
    if (changed) {
        if (up) {
            espos_ble_scan_suspend("setup portal is up (shared radio)");
        } else {
            espos_ble_scan_resume();
        }
    }
    xSemaphoreGiveRecursive(scan_mutex());
}

static void on_portal_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    espos_ble_portal_hold(id == ESPOS_EVENT_PORTAL_UP);
}

/* No network, no scanning: until the station has an address the gateway has
 * nowhere to post, and the airtime belongs to the association.
 *
 * Measured on an ESP32-C5 (one radio, coexistence in software) with the
 * default 160 ms window every 320 ms: every attempt to join failed with
 * reason 36 after 20 s, four times in a row, and the station associated
 * within 15 s of the setup portal suspending the scanner -- not because of
 * the portal, but because the scan had stopped. A dropped link would take
 * as long to come back. Same single-hold shape as the portal. */
static void net_hold(bool down)
{
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    taskENTER_CRITICAL(&s_suspend_lock);
    bool changed = (down != g.net_holds_scan);
    g.net_holds_scan = down;
    taskEXIT_CRITICAL(&s_suspend_lock);
    if (changed) {
        if (down) {
            espos_ble_scan_suspend("no network yet (shared radio)");
        } else {
            espos_ble_scan_resume();
        }
    }
    xSemaphoreGiveRecursive(scan_mutex());
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    net_hold(id == ESPOS_EVENT_NETWORK_DOWN);
}

/* No scanning while espos_ota_busy(): an update queued, being checked for,
 * downloading, verifying or about to reboot into. The image streams through
 * the same radio, and through the WiFi and lwIP buffers that stay in internal
 * RAM even with PSRAM on -- the RAM the scanner's advertisements and POSTs
 * also churn. The C5 gateway idles near 22 KB of free internal RAM and
 * already touches the 12 KB alarm with no update running. Polled from
 * gateway_task rather than driven by an event: a dropped "finished" event
 * after a failed install would leave the scanner off until the next reboot.
 * The cost is a lag of up to one loop pass, a POST included, before the hold
 * takes or lifts. Same single-hold shape as the portal and the network. */
static void ota_hold(bool busy)
{
    if (busy == g.ota_holds_scan) {
        return;
    }
    g.ota_holds_scan = busy;
    if (busy) {
        espos_ble_scan_suspend("update in progress (shared radio)");
    } else {
        espos_ble_scan_resume();
    }
}

static esp_err_t scan_suspend_locked(const char *reason);
static esp_err_t scan_resume_locked(void);

esp_err_t espos_ble_scan_suspend(const char *reason)
{
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    esp_err_t err = scan_suspend_locked(reason);
    xSemaphoreGiveRecursive(scan_mutex());
    return err;
}

esp_err_t espos_ble_scan_resume(void)
{
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    esp_err_t err = scan_resume_locked();
    xSemaphoreGiveRecursive(scan_mutex());
    return err;
}

static esp_err_t scan_suspend_locked(const char *reason)
{
    taskENTER_CRITICAL(&s_suspend_lock);
    unsigned before = g.scan_suspend_count++;
    taskEXIT_CRITICAL(&s_suspend_lock);
    /* Logged at INFO and naming the reason: a silent scanner is the symptom
     * of both this (deliberate) and a stolen GAP callback (a bug), and the
     * log is where someone will look to tell them apart. */
    ESP_LOGI(TAG, "scanning suspended (%s)%s", reason ? reason : "unspecified",
             before ? " [already suspended]" : "");
    if (before > 0) return ESP_OK;
    if (!g.running) return ESP_OK;
    return espos_ble_scan_stop();
}

static esp_err_t scan_resume_locked(void)
{
    taskENTER_CRITICAL(&s_suspend_lock);
    bool held = g.scan_suspend_count > 0;
    unsigned left = held ? --g.scan_suspend_count : 0;
    taskEXIT_CRITICAL(&s_suspend_lock);
    if (!held) {
        return ESP_OK; /* nothing was suspended; resuming is a no-op */
    }
    if (left > 0) {
        ESP_LOGI(TAG, "scanning still suspended (%u holder(s) left)", left);
        return ESP_OK;
    }
    if (!g.running) {
        ESP_LOGI(TAG, "scanning resumed (gateway not running; nothing to restart)");
        return ESP_OK;
    }
    /* Order matters. Whoever held the radio has almost certainly replaced the
     * one GAP callback Bluedroid keeps, and scan results are delivered
     * through it -- so restarting the scan without reclaiming first yields a
     * scanner that runs and reports nothing. */
    esp_err_t err = espos_ble_gap_reclaim();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not reclaim the GAP callback: %s; scan results would be lost",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "scanning resumed");
    return espos_ble_scan_start(g.active_scan, (uint16_t)g.scan_int, (uint16_t)g.scan_win);
}

bool espos_ble_scan_is_suspended(void) { return g.scan_suspend_count > 0; }

esp_err_t espos_ble_stop(void)
{
    if (!g.running) return ESP_OK;
    /* Under the scan mutex, so a hold being released on another task cannot
     * pass its g.running check and restart the scanner after this stops it. */
    xSemaphoreTakeRecursive(scan_mutex(), portMAX_DELAY);
    g.running = false;
    espos_ble_scan_stop();
    xSemaphoreGiveRecursive(scan_mutex());

    /* gateway_task polls g.running every 100 ms and deletes itself; wait for
     * it before freeing anything it touches. Wait on its own flag rather than
     * eTaskGetState(): once the task has called vTaskDelete(NULL) the handle
     * may already be freed, and inspecting it is undefined. */
    for (int i = 0; i < 40 && !g.task_exited; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!g.task_exited) {
        /* Leaking is the safe branch: freeing buffers a live task still walks
         * would be a use-after-free. Restore running so a later stop can try
         * again rather than short-circuiting on the !g.running guard. */
        ESP_LOGW(TAG, "gateway task did not exit; leaving its buffers alone");
        g.running = true;
        return ESP_ERR_TIMEOUT;
    }
    g.task = NULL;

    if (g.ws) {
        esp_websocket_client_stop(g.ws);
        esp_websocket_client_destroy(g.ws);
        g.ws = NULL;
        g.ws_connected = false;
    }

    /* Close any live session and release its allocations - a stop/start cycle
     * would otherwise orphan them and leave stale connection handles behind. */
    if (sess_lock()) {
        for (size_t i = 0; i < MAX_SESSIONS; i++) {
            if (!g.sessions[i].active) continue;
            espos_ble_gatt_disconnect(g.sessions[i].conn_handle);
            session_free(&g.sessions[i]);
        }
        sess_unlock();
    }

    if (g.lock) {
        vSemaphoreDelete(g.lock);
        g.lock = NULL;
    }
    if (g.sess_lock) {
        vSemaphoreDelete(g.sess_lock);
        g.sess_lock = NULL;
    }
    free(g.storage);
    g.storage = NULL;
    memset(&g.q, 0, sizeof(g.q));
    return ESP_OK;
}

esp_err_t espos_ble_get_status(espos_ble_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->enabled = g.running;
    out->scanning = espos_ble_is_scanning();
    out->scan_suspended = g.scan_suspend_count > 0;
    snprintf(out->mac, sizeof(out->mac), "%s", espos_ble_mac());
    out->scan_hits = espos_ble_scan_hits();
    out->adv_received = g.adv_received;
    out->adv_posted = g.adv_posted;
    /* Under the lock like every other q access: this runs on the httpd task
     * while the Bluetooth stack task is pushing.
     *
     * Failing to take it means the ring counters cannot be read at all, and a
     * partial adv_dropped -- contention drops without the ring's evictions --
     * would be an undercount indistinguishable from a real one on the public
     * endpoint. Report the failure instead of a plausible wrong number; every
     * holder is short (a push, a drain that copies out before it POSTs, this
     * read), so a timeout here means something is genuinely wrong. */
    if (!g.lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(g.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->adv_dropped = g.q.dropped;
    out->adv_pending = espos_ble_advq_count(&g.q);
    xSemaphoreGive(g.lock);
    /* Separately synchronised, so it is read outside the lock and summed in:
     * adv_dropped is the whole tally, both eviction and lock contention. */
    out->adv_dropped +=
        (uint32_t)atomic_load_explicit(&g.lock_drops, memory_order_relaxed);
    out->post_success = g.post_ok;
    out->post_fail = g.post_fail;
    out->ws_connected = g.ws_connected;
    out->gatt_sessions = espos_ble_gatt_active_count();
    out->gatt_max = g.max_gatt;
    return ESP_OK;
}

#endif /* CONFIG_BT_BLUEDROID_ENABLED */
