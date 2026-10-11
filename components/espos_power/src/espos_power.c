/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_power -- see espos_power.h.
 *
 * One low-priority task samples what the policy needs five times a second:
 * the network seam, the SignalK stream, the application's holds and whether
 * the running image is confirmed. Everything it decides is the policy's
 * (power_policy.c); this file only gathers the inputs and carries out a SLEEP.
 *
 * Sleeping is the one step that cannot be taken back, so it goes in a fixed
 * order: flush the stream while it is still connected, give the TCP stack a
 * moment to put the last frame on the wire, write the counters that survive
 * into RTC memory, then enter deep sleep with the interval as a timer wake.
 */
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_httpd.h"
#include "espos_net.h"
#include "espos_power.h"
#include "power_port.h"
#if ESPOS_POWER_HAVE_SK
#include "espos_sk.h"
#endif
#if ESPOS_POWER_HAVE_OTA
#include "espos_ota.h"
#endif

static const char *TAG = "espos_power";

/* espos_wifi is optional here and needs nothing from us: a weak reference,
 * NULL when it is not linked. A radio co-processor update is a firmware
 * transfer like an OTA, and a sleep in the middle of it cuts it short. */
extern bool espos_wifi_coproc_busy(void) __attribute__((weak));

#define TICK_MS 200
/* After espos_sk_flush(): every message is handed to the socket, which is not
 * the same as on the wire. On a live link lwIP sends within a few ms. */
#define TCP_GRACE_MS 150

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool started;
    atomic_uint holds;
    atomic_bool cfg_dirty;
    /* task-private */
    bool enabled;
    uint32_t interval_s;
    uint32_t flush_ms;
    espos_power_policy_cfg_t cfg;
    bool timer_wake;
    uint32_t wake_count;
    uint32_t last_awake_ms;
    bool unconfirmed; /* sticky false: a confirmed image does not become unconfirmed again */
    bool net_was_up;
    uint32_t net_up_since_ms;
    bool stream_was_up;
    uint32_t stream_up_since_ms;
    espos_power_why_t logged_why;
    bool sim_slept; /* the host refused to sleep: stop deciding */
    /* shared snapshot, under s_mux */
    espos_power_status_t snap;
} s = { .logged_why = ESPOS_POWER_WHY_MAX };

static uint32_t cfg_u32(const char *key, uint32_t fallback)
{
    int32_t v;
    return espos_config_get_i32(ESPOS_CFG_NS_POWER, key, &v) == ESP_OK && v >= 0 ? (uint32_t)v : fallback;
}

static void load_cfg(void)
{
    char mode[8] = "off";
    (void)espos_config_get_str(ESPOS_CFG_NS_POWER, ESPOS_CFG_POWER_MODE, mode, sizeof(mode), NULL);
    bool enabled = strcmp(mode, "cycle") == 0;
    s.interval_s = cfg_u32(ESPOS_CFG_POWER_INTERVAL_S, 300);
    s.flush_ms = cfg_u32(ESPOS_CFG_POWER_FLUSH_MS, 3000);
    s.cfg.window_ms = cfg_u32(ESPOS_CFG_POWER_WINDOW_S, 300) * 1000;
    s.cfg.awake_max_ms = cfg_u32(ESPOS_CFG_POWER_AWAKE_MAX_S, 30) * 1000;
    s.cfg.publish_ms = cfg_u32(ESPOS_CFG_POWER_PUBLISH_MS, 1500);
    if (enabled != s.enabled) {
        ESP_LOGI(TAG, "mode %s (sleep %u s, window %u s, wake at most %u s)", enabled ? "cycle" : "off",
                 (unsigned)s.interval_s, (unsigned)(s.cfg.window_ms / 1000), (unsigned)(s.cfg.awake_max_ms / 1000));
    }
    s.enabled = enabled;
}

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_POWER) == 0) {
        atomic_store(&s.cfg_dirty, true);
    }
}

/* Flush, wait for the wire, remember, sleep. Returns only where the platform
 * cannot sleep (the host). */
static esp_err_t go_to_sleep(const char *why)
{
    uint32_t awake_ms = espos_power_port_uptime_ms();
#if ESPOS_POWER_HAVE_SK
    espos_sk_ws_status_t ws;
    if (s.flush_ms > 0 && espos_sk_ws_get_status(&ws) == ESP_OK && ws.connected) {
        esp_err_t err = espos_sk_flush(s.flush_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush before sleep: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(TCP_GRACE_MS));
    }
#endif
    /* wake_count is the number of the wake this boot is (0 after a power-on),
     * so the next boot is one more. */
    espos_power_port_rtc_save(s.wake_count + 1, awake_ms);
    ESP_LOGI(TAG, "sleeping %u s after %u ms awake (%s)", (unsigned)s.interval_s, (unsigned)awake_ms, why);
    return espos_power_port_deep_sleep((uint64_t)s.interval_s * 1000000ULL);
}

static void power_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (atomic_exchange(&s.cfg_dirty, false)) {
            load_cfg();
            s.sim_slept = false; /* a new configuration is a new question */
        }
        if (s.sim_slept) {
            /* The platform refused to sleep (the host always does). Keep
             * reporting the decision that was refused instead of asking again
             * five times a second. */
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            continue;
        }
        uint32_t now = espos_power_port_uptime_ms();

        bool up = espos_net_is_up();
        if (up && !s.net_was_up) {
            s.net_up_since_ms = now;
        }
        s.net_was_up = up;

        bool have_stream = false;
        bool connected = false;
#if ESPOS_POWER_HAVE_SK
        espos_sk_ws_status_t ws;
        if (espos_sk_ws_get_status(&ws) == ESP_OK) {
            have_stream = ws.enabled;
            connected = ws.connected;
        }
#endif
        if (connected && !s.stream_was_up) {
            s.stream_up_since_ms = now;
        }
        s.stream_was_up = connected;

        if (s.unconfirmed) {
            s.unconfirmed = espos_power_port_image_unconfirmed();
        }

        uint32_t holds = atomic_load(&s.holds);
        espos_power_policy_in_t in = {
            .enabled = s.enabled && !s.sim_slept,
            .timer_wake = s.timer_wake,
            .image_unconfirmed = s.unconfirmed,
            .ota_busy =
#if ESPOS_POWER_HAVE_OTA
                espos_ota_busy() ||
#endif
                (espos_wifi_coproc_busy && espos_wifi_coproc_busy()),
            .net_up = up,
            .net_up_for_ms = up ? now - s.net_up_since_ms : 0,
            .have_stream = have_stream,
            .stream_connected = connected,
            .stream_up_for_ms = connected ? now - s.stream_up_since_ms : 0,
            .uptime_ms = now,
            .holds = holds,
        };
        espos_power_why_t why = ESPOS_POWER_WHY_OFF;
        espos_power_decision_t decision = espos_power_policy_decide(&s.cfg, &in, &why);

        portENTER_CRITICAL(&s_mux);
        s.snap.enabled = s.enabled;
        s.snap.interval_s = s.interval_s;
        s.snap.deadline_ms = espos_power_policy_deadline_ms(&s.cfg, s.timer_wake);
        s.snap.uptime_ms = now;
        s.snap.holds = holds;
        s.snap.decision = decision;
        s.snap.why = why;
        portEXIT_CRITICAL(&s_mux);

        if (why != s.logged_why) {
            s.logged_why = why;
            ESP_LOGI(TAG, "%s: %s", decision == ESPOS_POWER_SLEEP ? "sleep" : "awake", espos_power_why_str(why));
        }
        if (decision == ESPOS_POWER_SLEEP && go_to_sleep(espos_power_why_str(why)) != ESP_OK) {
            s.sim_slept = true;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ---------------------------------------------------------------- REST */

static esp_err_t power_get(httpd_req_t *req)
{
    espos_power_status_t st;
    if (espos_power_get_status(&st) != ESP_OK) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "not_started", "espos_power not started");
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_memory", "");
    }
    cJSON_AddStringToObject(j, "mode", st.enabled ? "cycle" : "off");
    cJSON_AddStringToObject(j, "decision", st.decision == ESPOS_POWER_SLEEP ? "sleep" : "stay");
    cJSON_AddStringToObject(j, "why", espos_power_why_str(st.why));
    cJSON_AddBoolToObject(j, "timer_wake", st.timer_wake);
    cJSON_AddNumberToObject(j, "wake_count", st.wake_count);
    cJSON_AddNumberToObject(j, "last_awake_ms", st.last_awake_ms);
    cJSON_AddNumberToObject(j, "interval_s", st.interval_s);
    cJSON_AddNumberToObject(j, "uptime_ms", st.uptime_ms);
    cJSON_AddNumberToObject(j, "deadline_ms", st.deadline_ms);
    cJSON_AddNumberToObject(j, "holds", st.holds);
    char *json = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!json) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_memory", "");
    }
    esp_err_t err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

/* ----------------------------------------------------------------- API */

esp_err_t espos_power_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    espos_net_status_t ns;
    if (!espos_config_is_ready() || espos_net_get_status(&ns) != ESP_OK) {
        ESP_LOGE(TAG, "espos_power_start: call espos_start_network() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    s.timer_wake = espos_power_port_timer_wake();
    if (s.timer_wake) {
        espos_power_port_rtc_load(&s.wake_count, &s.last_awake_ms);
    }
    s.unconfirmed = espos_power_port_image_unconfirmed();
    load_cfg();
    s.snap.timer_wake = s.timer_wake;
    s.snap.wake_count = s.wake_count;
    s.snap.last_awake_ms = s.last_awake_ms;

    static const httpd_uri_t uri = { .uri = "/api/v1/power", .method = HTTP_GET, .handler = power_get };
    esp_err_t err = espos_httpd_register(&uri);
    if (err != ESP_OK) {
        return err;
    }
    (void)espos_config_subscribe(on_config_change, NULL);
    s.started = true;
    if (xTaskCreate(power_task, "espos_power", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        s.started = false;
        return ESP_ERR_NO_MEM;
    }
    if (s.timer_wake) {
        ESP_LOGI(TAG, "wake %u (the last one took %u ms)", (unsigned)s.wake_count, (unsigned)s.last_awake_ms);
    }
    return ESP_OK;
}

void espos_power_hold(void)
{
    atomic_fetch_add(&s.holds, 1);
}

void espos_power_release(void)
{
    unsigned cur = atomic_load(&s.holds);
    while (cur > 0 && !atomic_compare_exchange_weak(&s.holds, &cur, cur - 1)) {
    }
}

esp_err_t espos_power_sleep_now(void)
{
    if (!s.started || espos_power_port_image_unconfirmed()) {
        return ESP_ERR_INVALID_STATE;
    }
    return go_to_sleep("requested");
}

esp_err_t espos_power_get_status(espos_power_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.started) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s.snap;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}
