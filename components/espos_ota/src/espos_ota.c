/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA task: manifest checks, installs, and the confirm/rollback policy for
 * a freshly booted image. One task, one command queue; blocking downloads
 * happen on this task so the HTTP server never stalls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_httpd.h"
#include "espos_httpd_sse.h"
#include "espos_net.h"
#include "espos_ota.h"
#include "espos_ota_manifest.h"
#include "ota_port.h"
/* The one WiFi-specific question left here — "is there no station configured
 * at all?" — decides whether a portal-only device confirms a new image
 * after a grace period instead of rolling back. Optional: a build without
 * espos_wifi (Ethernet, H-series) has no such state and never takes that
 * shortcut. */
#if ESPOS_OTA_HAVE_WIFI
#include "espos_wifi.h"
#endif

static const char *TAG = "espos_ota";

#define CMD_CHECK         1
#define CMD_INSTALL_URL   2
#define CMD_INSTALL_AVAIL 3
#define CMD_CONFIRM       4
#define CMD_ROLLBACK      5
#define CMD_STOP          6

/* Sized in Kconfig ("espOS OTA"); the short names keep the code readable. */
#define MANIFEST_MAX       CONFIG_ESPOS_OTA_MANIFEST_MAX
#define BOOT_CHECK_DELAY_S CONFIG_ESPOS_OTA_BOOT_CHECK_DELAY_S  /* let WiFi come up before the first manifest fetch */
#define NO_STA_GRACE_S     CONFIG_ESPOS_OTA_NO_STA_GRACE_S      /* confirm without station config after this */

typedef struct {
    int cmd;
    char url[ESPOS_OTA_URL_MAX];
} cmd_t;

static struct {
    SemaphoreHandle_t lock;
    QueueHandle_t q;
    TaskHandle_t task;
    espos_ota_state_t state;
    char last_error[96];
    espos_ota_build_t avail;
    bool have_avail;
    size_t received, total;
    uint32_t last_check_at;    /* uptime s, 0 = never */
    uint32_t next_check_at;
    bool ever_connected;
    bool confirmed_this_boot;
    bool rolling_back;      /* from the rollback's check until its reboot */
    char key_fp[17];        /* "" until known, or when the image has no signature block */
    bool key_fp_done;       /* OTA task only */
    /* config snapshot */
    char manifest_url[168];
    /* What the last check actually fetched. With manifest_src = "signalk"
     * the configured URL is empty, so reporting it would show nothing where
     * a real URL was used -- and an operator reading /api/v1/ota to find out
     * where a device looks would be told "nowhere". */
    char manifest_eff[168];
    char manifest_src[12];
    char manifest_path[128];
    char channel[16];
    bool auto_check, auto_install, allow_insecure;
    int32_t check_h, confirm_tmo_s;
    bool cfg_dirty;
} s;

static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

const char *espos_ota_state_name(espos_ota_state_t st)
{
    switch (st) {
    case ESPOS_OTA_IDLE: return "idle";
    case ESPOS_OTA_CHECKING: return "checking";
    case ESPOS_OTA_AVAILABLE: return "available";
    case ESPOS_OTA_DOWNLOADING: return "downloading";
    case ESPOS_OTA_VERIFYING: return "verifying";
    case ESPOS_OTA_READY: return "ready";
    case ESPOS_OTA_FAILED: return "failed";
    default: return "?";
    }
}

static void load_config(void)
{
    lock();
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_MANIFEST_URL, s.manifest_url, sizeof(s.manifest_url), NULL);
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_MANIFEST_SRC, s.manifest_src, sizeof(s.manifest_src), NULL);
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_MANIFEST_PATH, s.manifest_path, sizeof(s.manifest_path), NULL);
    espos_config_get_str(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CHANNEL, s.channel, sizeof(s.channel), NULL);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_AUTO_CHECK, &s.auto_check);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_AUTO_INSTALL, &s.auto_install);
    espos_config_get_bool(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_ALLOW_INSECURE, &s.allow_insecure);
    espos_config_get_i32(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CHECK_H, &s.check_h);
    espos_config_get_i32(ESPOS_CFG_NS_OTA, ESPOS_CFG_OTA_CONFIRM_TMO_S, &s.confirm_tmo_s);
    if (!s.channel[0]) {
        strcpy(s.channel, "stable");
    }
    s.cfg_dirty = false;
    unlock();
}

static void on_config(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_OTA) == 0) {
        s.cfg_dirty = true;
    }
}

static void publish(void)
{
    char *j = espos_ota_status_json();
    if (j) {
        espos_httpd_sse_publish("ota", j);
        free(j);
    }
}

static void set_state(espos_ota_state_t st, const char *err)
{
    lock();
    s.state = st;
    if (err) {
        snprintf(s.last_error, sizeof(s.last_error), "%s", err);
    } else if (st != ESPOS_OTA_FAILED) {
        s.last_error[0] = '\0';
    }
    unlock();
    publish();
}

static void progress_cb(size_t received, size_t total, void *arg)
{
    (void)arg;
    bool notify = false;
    lock();
    /* publish every ~32 KiB or on completion, not on every chunk */
    if (received == total || received / 32768 != s.received / 32768) {
        notify = true;
    }
    s.received = received;
    s.total = total;
    unlock();
    if (notify) {
        publish();
    }
}

/* Quiesce anything that cannot miss a deadline while an image downloads.
 *
 * Draining a multi-megabyte image saturates the core the network stack runs
 * on. On a board that also runs a real-time consumer -- esp-sr's AFE on the
 * P4 cockpit panel feeds WakeNet at 16 kHz from two microphones -- that
 * consumer misses its deadline, the idle task is starved, and IDF's task
 * watchdog aborts the firmware MID-DOWNLOAD. Measured: 100% reproducible on a
 * 4.4 MB image, over both HTTPS and plain HTTP.
 *
 * Weak so espos_ota keeps no dependency on espos_voice (or on whatever else a
 * firmware wants to park): the component that owns the real-time work provides
 * the strong definition. A build with nothing to quiesce links these no-ops.
 *
 * Called from the OTA task around the download only; resume ALWAYS runs,
 * including on a failed install, so a failure cannot leave a device deaf.
 */
__attribute__((weak)) void espos_ota_quiesce_hook(void) {}
__attribute__((weak)) void espos_ota_resume_hook(void) {}

/*
 * Where to fetch the manifest from.
 *
 * With ota.manifest_src = "signalk" the URL is derived from whichever server
 * this device is already talking to, so a fresh device that finds its server
 * also finds its updates and nothing has to be typed per device -- and a
 * server that moves does not strand a fleet on a stale URL.
 *
 * espos_sk is an OPTIONAL dependency: a firmware can do OTA with no SignalK
 * at all, and hard-requiring it to offer this one convenience would be the
 * wrong trade. The weak symbol is what keeps that true; espos_sk provides the
 * strong one (src/sk_ota_url.c). Same shape as the wall-clock hook in
 * espos_httpd, and the same trap: the strong definition only wins if the
 * linker pulls that object in, which is why espos_sk builds WHOLE_ARCHIVE.
 */
__attribute__((weak)) esp_err_t espos_ota_server_url_hook(const char *path, char *out, size_t n)
{
    (void)path;
    (void)out;
    (void)n;
    return ESP_ERR_NOT_SUPPORTED;
}

static void do_check(void)
{
    char url[168], channel[16], src[12], path[128];
    bool insecure, auto_install;
    lock();
    snprintf(url, sizeof(url), "%s", s.manifest_url);
    snprintf(src, sizeof(src), "%s", s.manifest_src);
    snprintf(path, sizeof(path), "%s", s.manifest_path);
    snprintf(channel, sizeof(channel), "%s", s.channel);
    insecure = s.allow_insecure;
    auto_install = s.auto_install;
    s.last_check_at = espos_ota_port_uptime_s();
    s.next_check_at = s.last_check_at + (uint32_t)s.check_h * 3600;
    unlock();

    if (strcmp(src, "signalk") == 0) {
        esp_err_t uerr = espos_ota_server_url_hook(path, url, sizeof(url));
        if (uerr == ESP_ERR_NOT_SUPPORTED) {
            /* Configured to ask a server this firmware cannot talk to. Said
             * plainly rather than falling back to manifest_url, which would
             * silently check the wrong place. */
            set_state(ESPOS_OTA_FAILED, "manifest source is signalk, but this firmware has no SignalK client");
            return;
        }
        if (uerr != ESP_OK) {
            /* No server selected yet -- ordinary at boot, before discovery
             * has run. Not an error state worth alarming about; the next
             * scheduled check will find one. */
            set_state(ESPOS_OTA_FAILED, "no SignalK server selected yet");
            return;
        }
    }

    if (!url[0]) {
        set_state(ESPOS_OTA_FAILED, "no manifest URL configured");
        return;
    }
    /* Whatever we are about to fetch, that is what status should report. */
    lock();
    snprintf(s.manifest_eff, sizeof(s.manifest_eff), "%s", url);
    unlock();
    set_state(ESPOS_OTA_CHECKING, NULL);
    char *body = NULL;
    size_t len = 0;
    int status = 0;
    esp_err_t err = espos_ota_port_fetch(url, insecure, MANIFEST_MAX, &body, &len, &status);
    if (err != ESP_OK) {
        char e[96];
        if (status) {
            snprintf(e, sizeof(e), "manifest: HTTP %d", status);
        } else {
            snprintf(e, sizeof(e), "manifest: %s", esp_err_to_name(err));
        }
        set_state(ESPOS_OTA_FAILED, e);
        return;
    }
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    espos_ota_build_t b;
    err = espos_ota_manifest_pick(body, len, url, info.project, CONFIG_IDF_TARGET, channel, info.version, &b);
    free(body);
    if (err == ESP_ERR_NOT_FOUND) {
        lock();
        s.have_avail = false;
        unlock();
        char e[96];
        snprintf(e, sizeof(e), "no build for %s/%s in the manifest", CONFIG_IDF_TARGET, channel);
        set_state(ESPOS_OTA_FAILED, e);
        return;
    }
    if (err != ESP_OK) {
        set_state(ESPOS_OTA_FAILED, "manifest: malformed");
        return;
    }
    lock();
    s.avail = b;
    s.have_avail = true;
    unlock();
    if (!b.newer) {
        ESP_LOGI(TAG, "manifest: %s is the newest for %s/%s, running %s", b.version, CONFIG_IDF_TARGET, channel, info.version);
        set_state(ESPOS_OTA_IDLE, NULL);
        return;
    }
    ESP_LOGI(TAG, "manifest: %s available (running %s)%s", b.version, info.version, auto_install ? ", installing" : "");
    set_state(ESPOS_OTA_AVAILABLE, NULL);
    espos_event_ota_t ev = { 0 };
    snprintf(ev.version, sizeof(ev.version), "%s", b.version);
    (void)espos_event_post(ESPOS_EVENT_OTA_AVAILABLE, &ev, sizeof(ev));
    if (auto_install) {
        cmd_t c = { .cmd = CMD_INSTALL_AVAIL };
        xQueueSend(s.q, &c, 0);
    }
}

/* espos_wifi is optional here (and requires nothing from us), so a weak
 * reference: NULL when it is not linked. */
extern bool espos_wifi_coproc_busy(void) __attribute__((weak));

/* A radio co-processor update ends in a restart, which would cut an install
 * short or land a rollback mid-flash. */
static bool coproc_busy(void)
{
    return espos_wifi_coproc_busy && espos_wifi_coproc_busy();
}

static void do_install(const char *url)
{
    bool insecure;
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    lock();
    insecure = s.allow_insecure;
    s.received = 0;
    s.total = 0;
    unlock();
    /* Claimed before the check: espos_wifi claims its update before checking
     * espos_ota_busy(), so two racing starts cannot both miss each other.
     * Checked here too because auto_install queues straight onto the task,
     * past send_cmd(). */
    set_state(ESPOS_OTA_DOWNLOADING, NULL);
    if (coproc_busy()) {
        set_state(ESPOS_OTA_FAILED, "a radio co-processor update is running");
        return;
    }
    char err_text[96] = "";
    (void)espos_event_post(ESPOS_EVENT_UPDATE_STARTED, NULL, 0);
    espos_ota_quiesce_hook();
    esp_err_t err = espos_ota_port_install(url, insecure, info.project, progress_cb, NULL, err_text, sizeof(err_text));
    espos_ota_resume_hook();
    (void)espos_event_post(ESPOS_EVENT_UPDATE_FINISHED, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install failed: %s", err_text);
        set_state(ESPOS_OTA_FAILED, err_text);
        return;
    }
    set_state(ESPOS_OTA_READY, NULL);
    ESP_LOGW(TAG, "update installed, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));    /* let the status/SSE reach the client */
    espos_ota_port_reboot();
    /* Only the host simulator returns from here. */
    set_state(ESPOS_OTA_IDLE, NULL);
}

/* Confirm-or-rollback policy for a PENDING_VERIFY image; called once a second. */
static void tick_confirm(void)
{
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    if (!info.pending_verify || s.confirmed_this_boot) {
        return;
    }
    uint32_t up = espos_ota_port_uptime_s();
    bool connected = espos_net_is_up();
    bool no_station = false;
#if ESPOS_OTA_HAVE_WIFI
    espos_wifi_status_t w;
    if (espos_wifi_get_status(&w) == ESP_OK) {
        no_station = w.sm.state == ESPOS_WIFI_ST_UNCONFIGURED || w.sm.state == ESPOS_WIFI_ST_DISABLED;
    }
#endif
    if (connected || (no_station && up >= NO_STA_GRACE_S)) {
        ESP_LOGI(TAG, "new image confirmed (%s), rollback cancelled", connected ? "network up" : "no station configured");
        if (espos_ota_port_mark_valid() == ESP_OK) {
            s.confirmed_this_boot = true;
            publish();
        }
        return;
    }
    if (up >= (uint32_t)s.confirm_tmo_s) {
        ESP_LOGE(TAG, "new image could not reach the network in %u s: rolling back", (unsigned)up);
        set_state(ESPOS_OTA_FAILED, "rollback: network not reached in time");
        espos_ota_port_mark_invalid_and_reboot();
        s.confirmed_this_boot = true;   /* sim: do not repeat */
    }
}

/* The registry names each project's key by this fingerprint (signingKeyId),
 * so a fleet manager can tell before installing that an update signed with
 * another key would be refused. Here rather than in espos_ota_port_info:
 * the digest wants ~1.5 KB of stack, which this task has and an httpd handler
 * may not. A transient flash error is retried on the next tick, and a retry
 * that lands publishes, so an SSE client that saw null does not keep it. */
static void read_key_fp(bool announce)
{
    char fp[17];
    esp_err_t err = espos_ota_port_key_fp(fp);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "running image carries no signature block");
    }
    lock();
    snprintf(s.key_fp, sizeof(s.key_fp), "%s", fp);
    unlock();
    s.key_fp_done = true;
    if (announce) {
        publish();
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    load_config();
    read_key_fp(false);
    uint32_t boot_check_at = espos_ota_port_uptime_s() + BOOT_CHECK_DELAY_S;
    bool boot_check_done = false;
    while (1) {
        cmd_t c;
        if (xQueueReceive(s.q, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s.cfg_dirty) {
                load_config();      /* a PUT just before the command must count */
            }
            switch (c.cmd) {
            case CMD_CHECK: do_check(); break;
            case CMD_INSTALL_URL: do_install(c.url); break;
            case CMD_INSTALL_AVAIL: {
                char url[ESPOS_OTA_URL_MAX];
                lock();
                bool have = s.have_avail;
                snprintf(url, sizeof(url), "%s", s.avail.url);
                unlock();
                if (have) {
                    do_install(url);
                } else {
                    set_state(ESPOS_OTA_FAILED, "nothing to install: check first");
                }
                break;
            }
            case CMD_CONFIRM:
                if (espos_ota_port_mark_valid() == ESP_OK) {
                    s.confirmed_this_boot = true;
                }
                publish();
                break;
            case CMD_ROLLBACK:
                /* Claimed before the check, as do_install claims its state.
                 * The state a rollback reports is FAILED, which
                 * espos_ota_busy() does not count, so the claim is its own
                 * flag, held until the reboot. */
                lock();
                s.rolling_back = true;
                unlock();
                if (coproc_busy()) {
                    lock();
                    s.rolling_back = false;
                    unlock();
                    set_state(ESPOS_OTA_FAILED, "a radio co-processor update is running");
                    break;
                }
                ESP_LOGW(TAG, "rollback requested");
                set_state(ESPOS_OTA_FAILED, "rollback requested");
                espos_ota_port_mark_invalid_and_reboot();
                /* Back here only without an image to roll back to, or on the
                 * host simulator. */
                lock();
                s.rolling_back = false;
                unlock();
                break;
            case CMD_STOP:
                s.task = NULL;
                vTaskDelete(NULL);
                return;
            }
            continue;
        }
        if (s.cfg_dirty) {
            load_config();
        }
        if (!s.key_fp_done) {
            read_key_fp(true);
        }
        tick_confirm();
        /* periodic manifest checks: after boot once the network is up, then every check_h */
        bool up = espos_net_is_up();
        uint32_t now = espos_ota_port_uptime_s();
        lock();
        bool auto_check = s.auto_check && s.manifest_url[0];
        bool due = auto_check && up && ((!boot_check_done && now >= boot_check_at) || (boot_check_done && s.next_check_at && now >= s.next_check_at));
        bool busy = s.state == ESPOS_OTA_DOWNLOADING || s.state == ESPOS_OTA_VERIFYING || s.state == ESPOS_OTA_READY;
        unlock();
        if (due && !busy) {
            boot_check_done = true;
            do_check();
        }
    }
}

esp_err_t espos_ota_register_api(void);

esp_err_t espos_ota_start(void)
{
    if (s.task) {
        return ESP_OK;
    }
    /* The API goes onto the HTTP server and the confirm/rollback policy
     * watches the network; neither can be retrofitted later. */
    if (!espos_httpd_handle()) {
        ESP_LOGE(TAG, "espos_ota_start: call espos_httpd_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    espos_net_status_t net;
    if (espos_net_get_status(&net) != ESP_OK) {
        ESP_LOGE(TAG, "espos_ota_start: call espos_net_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.q = xQueueCreate(4, sizeof(cmd_t));
        if (!s.lock || !s.q) {
            return ESP_ERR_NO_MEM;
        }
        espos_config_subscribe(on_config, NULL);
        esp_err_t err = espos_ota_register_api();
        if (err != ESP_OK) {
            return err;
        }
    }
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    ESP_LOGI(TAG, "running %s %s in %s (%s)%s", info.project, info.version, info.slot, info.state,
             info.rolled_back ? " — previous update was rolled back" : "");
    /* 8 KB words on the simulator would overflow the 16-bit depth; bytes on chips. */
    const uint32_t stack = configMINIMAL_STACK_SIZE > 3072 ? configMINIMAL_STACK_SIZE * 2 : CONFIG_ESPOS_OTA_STACK_SIZE;
    if (xTaskCreate(ota_task, "espos_ota", stack, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void espos_ota_stop(void)
{
    if (s.task) {
        cmd_t c = { .cmd = CMD_STOP };
        xQueueSend(s.q, &c, portMAX_DELAY);
        while (s.task) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t send_cmd(int cmd, const char *url)
{
    if (!s.q) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((cmd == CMD_INSTALL_URL || cmd == CMD_INSTALL_AVAIL || cmd == CMD_ROLLBACK) && coproc_busy()) {
        return ESP_ERR_NOT_FINISHED;
    }
    lock();
    bool busy = s.state == ESPOS_OTA_CHECKING || s.state == ESPOS_OTA_DOWNLOADING || s.state == ESPOS_OTA_VERIFYING || s.state == ESPOS_OTA_READY;
    unlock();
    if (busy && cmd != CMD_CONFIRM) {
        return ESP_ERR_INVALID_STATE;
    }
    cmd_t c = { .cmd = cmd };
    if (url) {
        snprintf(c.url, sizeof(c.url), "%s", url);
    }
    return xQueueSend(s.q, &c, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t espos_ota_check_now(void)
{
    return send_cmd(CMD_CHECK, NULL);
}

esp_err_t espos_ota_install_url(const char *url)
{
    if (!url || strlen(url) >= ESPOS_OTA_URL_MAX || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd(CMD_INSTALL_URL, url);
}

esp_err_t espos_ota_install_available(void)
{
    lock();
    bool have = s.have_avail;
    unlock();
    if (!have) {
        return ESP_ERR_NOT_FOUND;
    }
    return send_cmd(CMD_INSTALL_AVAIL, NULL);
}

esp_err_t espos_ota_confirm(void)
{
    return send_cmd(CMD_CONFIRM, NULL);
}

esp_err_t espos_ota_rollback(void)
{
    return send_cmd(CMD_ROLLBACK, NULL);
}

bool espos_ota_busy(void)
{
    if (!s.lock) {
        return false;
    }
    lock();
    bool busy = s.state == ESPOS_OTA_CHECKING || s.state == ESPOS_OTA_DOWNLOADING || s.state == ESPOS_OTA_VERIFYING ||
                s.state == ESPOS_OTA_READY || s.rolling_back;
    unlock();
    /* The task polls its queue once a second, and POST /ota has answered 202
     * by then: a request still waiting in the queue is an update too. */
    return busy || (s.q && uxQueueMessagesWaiting(s.q) > 0);
}

static void json_str(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 2 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (o + 3 >= n) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* Every field at its escaped maximum, the available build included, is ~2.2 KiB,
 * past the 2048 this used to be. */
#define STATUS_JSON_MAX 2560

/* snprintf returns what it WOULD have written; left unclamped, the next
 * append's STATUS_JSON_MAX - n goes negative and becomes a huge size_t. */
static int clamp_len(int n)
{
    if (n < 0) {
        return 0;
    }
    return n >= STATUS_JSON_MAX ? STATUS_JSON_MAX - 1 : n;
}

char *espos_ota_status_json(void)
{
    espos_ota_port_info_t info;
    espos_ota_port_info(&info);
    char *out = malloc(STATUS_JSON_MAX);
    if (!out) {
        return NULL;
    }
    lock();
    char err[200], url[520], notes[280], murl[340];
    json_str(err, sizeof(err), s.last_error);
    json_str(url, sizeof(url), s.avail.url);
    json_str(notes, sizeof(notes), s.avail.notes);
    json_str(murl, sizeof(murl),
             s.manifest_eff[0] ? s.manifest_eff : s.manifest_url);
    uint32_t now = espos_ota_port_uptime_s();
    int n;
    char last[16] = "null", next[16] = "null";
    char key_fp[20] = "null";
    if (s.key_fp[0]) {
        snprintf(key_fp, sizeof(key_fp), "\"%s\"", s.key_fp);
    }
    if (s.last_check_at) {
        snprintf(last, sizeof(last), "%u", (unsigned)(now - s.last_check_at));
    }
    if (s.next_check_at && s.auto_check) {
        snprintf(next, sizeof(next), "%d", (int)(s.next_check_at - now));
    }
    n = snprintf(out, STATUS_JSON_MAX,
                 "{\"state\":\"%s\",\"last_error\":\"%s\","
                 "\"running\":{\"version\":\"%s\",\"project\":\"%s\",\"target\":\"%s\",\"slot\":\"%s\",\"image_state\":\"%s\","
                 "\"pending_verify\":%s,\"confirmed\":%s,\"other_slot\":\"%s\",\"other_version\":\"%s\",\"rolled_back\":%s,"
                 "\"built\":\"%s %s\",\"idf\":\"%s\",\"key_fp\":%s},"
                 "\"manifest\":{\"url\":\"%s\",\"channel\":\"%s\",\"auto_check\":%s,\"auto_install\":%s,"
                 "\"last_check_s\":%s,\"next_check_s\":%s},"
                 "\"progress\":{\"received\":%u,\"total\":%u}",
                 espos_ota_state_name(s.state), err,
                 info.version, info.project, CONFIG_IDF_TARGET, info.slot, info.state,
                 info.pending_verify && !s.confirmed_this_boot ? "true" : "false",
                 s.confirmed_this_boot || !info.pending_verify ? "true" : "false",
                 info.other_slot, info.other_version, info.rolled_back ? "true" : "false",
                 info.date, info.time, info.idf, key_fp,
                 murl, s.channel, s.auto_check ? "true" : "false", s.auto_install ? "true" : "false",
                 last, next,
                 (unsigned)s.received, (unsigned)s.total);
    n = clamp_len(n);
    if (s.have_avail) {
        n += snprintf(out + n, STATUS_JSON_MAX - n, ",\"available\":{\"version\":\"%s\",\"url\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"notes\":\"%s\",\"newer\":%s}",
                      s.avail.version, url, (unsigned)s.avail.size, s.avail.sha256, notes, s.avail.newer ? "true" : "false");
    } else {
        n += snprintf(out + n, STATUS_JSON_MAX - n, ",\"available\":null");
    }
    n = clamp_len(n);
    snprintf(out + n, STATUS_JSON_MAX - n, "}");
    unlock();
    return out;
}
