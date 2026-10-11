/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/system/{info,reboot,factory-reset} — protected — and the public
 * /api/v1/system/ping. info carries the health policy's reset record
 * (last_reset) when the previous boot ended in one.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
/* The hardware block below is device-only: the linux target has no flash chip
 * to size, no PSRAM and no MAC, and espos_httpd builds for it (two host test
 * projects link this file). */
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#endif
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_timer.h"
#endif
#include "cJSON.h"

#include "espos_config.h"
#include "espos_health.h"
#include "espos_httpd.h"
#include "espos_httpd_priv.h"

static const char *TAG = "espos_httpd";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32: return "esp32";
    case CHIP_ESP32S2: return "esp32s2";
    case CHIP_ESP32S3: return "esp32s3";
    case CHIP_ESP32C3: return "esp32c3";
    case CHIP_ESP32C2: return "esp32c2";
    case CHIP_ESP32C6: return "esp32c6";
    case CHIP_ESP32H2: return "esp32h2";
    case CHIP_ESP32P4: return "esp32p4";
    case CHIP_ESP32C61: return "esp32c61";
    case CHIP_ESP32C5: return "esp32c5";
    case CHIP_ESP32H21: return "esp32h21";
    case CHIP_ESP32H4: return "esp32h4";
    default: return "unknown";
    }
}

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

/* "last_reset": the record espos_health's policy left when it restarted the
 * device, or null. Its message is a consumer's free text, so this goes through
 * cJSON rather than snprintf: a quote in it must not break the document. */
static void add_last_reset(cJSON *root)
{
    espos_health_reset_record_t rec;
    if (!espos_health_last_reset(&rec)) {
        cJSON_AddNullToObject(root, "last_reset");
        return;
    }
    cJSON *lr = cJSON_AddObjectToObject(root, "last_reset");
    if (!lr) {
        return;
    }
    cJSON_AddStringToObject(lr, "reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(lr, "health_key", rec.key);
    cJSON_AddStringToObject(lr, "message", rec.message);
    cJSON_AddNumberToObject(lr, "min_free_heap_before", rec.min_free_heap);
    cJSON_AddNumberToObject(lr, "min_internal_before", rec.min_internal);
    cJSON_AddNumberToObject(lr, "largest_block_before", rec.largest_block);
    cJSON_AddNumberToObject(lr, "uptime_before_s", rec.uptime_s);
    if (rec.unix_ms > 0) {
        time_t secs = (time_t)(rec.unix_ms / 1000);
        struct tm tm;
        char iso[32];
        gmtime_r(&secs, &tm);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
        cJSON_AddStringToObject(lr, "at", iso);
    } else {
        cJSON_AddNullToObject(lr, "at"); /* the clock was never set that boot */
    }
}

/**
 * The wall clock, if this firmware has one. espos_time provides the strong
 * definition (its src/log_wallclock.c); this weak one says "no clock", which
 * is what a build without that component gets.
 *
 * A hook rather than a call: espos_time depends on espos_httpd for its own
 * /time endpoints, so espos_httpd naming espos_time would close a cycle. The
 * override direction costs nothing and keeps /system/info honest either way.
 */
__attribute__((weak)) bool espos_httpd_wallclock_hook(bool *synced, const char **source, int64_t *unix_ms)
{
    (void)synced;
    (void)source;
    (void)unix_ms;
    return false;
}

/* The board label, for the same reason and by the same route as the wallclock
 * hook above: only espos_core holds it (espos_start_opts_t.board), and
 * espos_core STARTS espos_httpd, so naming it here would close a cycle. A
 * build without espos_core -- a firmware that registers its own endpoints and
 * never calls espos_start() -- gets the weak stub and no board row.
 *
 * espos_core/src/espos_core.c provides the strong definition. That object is
 * always in the link because app_main calls espos_start(), which lives there,
 * so espos_core needs no WHOLE_ARCHIVE. A component whose strong definition
 * sits in an object nothing else references does need it (espos_time,
 * espos_sk, espos_voice); without it the linker keeps this stub and the field
 * silently never appears. That is not hypothetical: it is exactly what
 * happened to the wallclock hook (#49), where both halves compiled, both
 * linked, and the two endpoints disagreed on a running device.
 */
__attribute__((weak)) const char *espos_httpd_board_hook(void)
{
    return NULL;
}

/* The radio co-processor, by the same route again: only espos_wifi links
 * esp_hosted, and espos_wifi REQUIRES espos_httpd, so naming it here would
 * close the cycle the other two hooks exist to avoid.
 *
 * espos_wifi/src/hosted_watchdog.c holds the strong definition, in an object
 * espos_wifi.c already calls into (espos_wifi_hosted_watchdog_start), so
 * espos_wifi needs no WHOLE_ARCHIVE -- the same reasoning as espos_core above,
 * and the trap #49 fell into when it did not hold. A build with no hosted
 * radio -- every native-radio target, and the linux host -- keeps this stub and
 * the object is simply absent from the document. */
__attribute__((weak)) bool espos_httpd_coprocessor_hook(espos_httpd_coproc_t *out)
{
    (void)out;
    return false;
}

/* "time": what the device believes the wall clock says and where it learned
 * it. Always present, so a client never has to guess whether the firmware has
 * the component; `source: "none"` and `now: 0` is the honest answer for a
 * device that has not been told the time yet. */
static void add_time(cJSON *root)
{
    bool synced = false;
    const char *source = "none";
    int64_t unix_ms = 0;
    (void)espos_httpd_wallclock_hook(&synced, &source, &unix_ms);
    cJSON *t = cJSON_AddObjectToObject(root, "time");
    if (!t) {
        return;
    }
    cJSON_AddBoolToObject(t, "synced", synced);
    cJSON_AddStringToObject(t, "source", source);
    cJSON_AddNumberToObject(t, "now", (double)unix_ms);
}

/* Absent on a chip that is its own radio -- most of them -- and absent too
 * while a co-processor has not answered yet. Those two are NOT
 * distinguishable from the document, which is the honest shape: a
 * co-processor that answers without naming a version is present with
 * "0.0.0", so the only ambiguity left is "no radio chip" versus "one that
 * never replied", and both mean there is nothing to report.
 *
 * `hw` NULL means "hardware" does not exist yet and is created only if the
 * hook answers -- the linux host, where nothing else would go in it. */
static void add_coprocessor(cJSON *j, cJSON *hw)
{
    espos_httpd_coproc_t cp;
    memset(&cp, 0, sizeof(cp));
    if (!espos_httpd_coprocessor_hook(&cp)) {
        return;
    }
    if (!hw) {
        hw = cJSON_AddObjectToObject(j, "hardware");
        if (!hw) {
            return;
        }
    }
    cJSON *co = cJSON_AddObjectToObject(hw, "coprocessor");
    if (!co) {
        return;
    }
    cJSON_AddStringToObject(co, "version", cp.version);
    cJSON_AddStringToObject(co, "host_version", cp.host_version);
    if (cp.target[0]) {
        cJSON_AddStringToObject(co, "target", cp.target);
    }
    /* The answer to "is this the thing causing my RPC timeouts", decided
     * where both numbers are known rather than left to every client to
     * compare two version strings correctly. */
    cJSON_AddBoolToObject(co, "stale", cp.stale);
}

/* Which board is this? -- the question someone with a drawer of dev boards
 * actually asks, answered from what the chip and the build already know.
 *
 * esp_chip_info() was already being called for the model and core count; its
 * `features` bitmask was read by nothing, and it is the part that says which
 * radios exist. The rest is one call each. None of it needed new plumbing,
 * which is why it is here rather than in a component of its own.
 *
 * Deliberately absent:
 *   - display size, touch: espOS has no display concept at all. A firmware
 *     that has a panel knows its geometry and can add a row of its own
 *     (docs/ui.md, registerPage); espos_httpd inventing one would be a
 *     abstraction with exactly zero implementations in this repo.
 *   - "BLE 5.0", "WiFi 6": the feature bits say WHETHER, not WHICH. A version
 *     would be a hardcoded table of datasheet facts keyed on chip model --
 *     espOS asserting something it cannot measure, wrong the first time a
 *     revision changes.
 *   - vendor and board model: nothing in the silicon carries it. The MAC's OUI
 *     is Espressif's (the module maker), and the USER_DATA efuse a vendor
 *     could burn an id into is blank on every board we have. Only the firmware
 *     knows, so it says: espos_start_opts_t.board, reported below when given.
 */
static void add_hardware(cJSON *j, const esp_chip_info_t *chip)
{
#if CONFIG_IDF_TARGET_LINUX
    /* Nothing else here is meaningful on the host, and a "hardware" object
     * full of zeros would be worse than its absence. The co-processor is the
     * exception: it comes from a hook, so the host test can link a strong one
     * and cover both the JSON and the weak/strong override (#168). With no
     * hook answering, "hardware" stays absent, as before. */
    (void)chip;
    add_coprocessor(j, NULL);
#else
    cJSON *hw = cJSON_AddObjectToObject(j, "hardware");
    if (!hw) {
        return;
    }

    /* The base MAC, which is the device's identity: espos_net derives the
     * short id and the default hostname from it, and it is what a server's
     * DHCP lease list shows. Six octets, lower case, colon-separated -- the
     * form every other tool prints, so it can be pasted into a search. */
    uint8_t mac[6] = { 0 };
    if (esp_base_mac_addr_get(mac) == ESP_OK || esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        cJSON_AddStringToObject(hw, "mac", buf);
    }

    /* Compile-time, not esp_clk_cpu_freq(): that lives in esp_private/ and
     * espOS's public-header rules keep private IDF headers out. This is the
     * frequency the build asked for, which is the one a reader wants to
     * compare against another board's -- a P4 at 360 MHz next to a C6 at 160. */
    cJSON_AddNumberToObject(hw, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK) {
        cJSON_AddNumberToObject(hw, "flash_bytes", (double)flash);
    }

    /* Totals, not free -- free heap is already reported and moves every
     * second; the totals are what distinguishes two boards with the same chip.
     * PSRAM is the one that matters: a P4 with 32 MB and one with none run the
     * same firmware very differently, and only this tells them apart. */
    cJSON_AddNumberToObject(hw, "ram_internal_bytes", (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    cJSON_AddNumberToObject(hw, "ram_psram_bytes", (double)psram);

    /* What the radios are, from the bitmask esp_chip_info() was already
     * filling in. An array rather than booleans: a reader scanning for "ble"
     * does not have to know which flags exist, and a chip that grows one does
     * not need a schema change. */
    cJSON *f = cJSON_AddArrayToObject(hw, "features");
    if (f) {
        if (chip->features & CHIP_FEATURE_WIFI_BGN) {
            cJSON_AddItemToArray(f, cJSON_CreateString("wifi"));
        }
        if (chip->features & CHIP_FEATURE_BLE) {
            cJSON_AddItemToArray(f, cJSON_CreateString("ble"));
        }
        if (chip->features & CHIP_FEATURE_BT) {
            cJSON_AddItemToArray(f, cJSON_CreateString("bt-classic"));
        }
        if (chip->features & CHIP_FEATURE_IEEE802154) {
            cJSON_AddItemToArray(f, cJSON_CreateString("802.15.4"));
        }
        if (chip->features & CHIP_FEATURE_EMB_FLASH) {
            cJSON_AddItemToArray(f, cJSON_CreateString("embedded-flash"));
        }
        if (chip->features & CHIP_FEATURE_EMB_PSRAM) {
            cJSON_AddItemToArray(f, cJSON_CreateString("embedded-psram"));
        }
    }

    /* Only if the firmware said. Omitted rather than blank: a UI can then show
     * the row or leave it out, instead of rendering an empty value that looks
     * like a device that failed to report. */
    const char *board = espos_httpd_board_hook();
    if (board && board[0]) {
        cJSON_AddStringToObject(hw, "board", board);
    }

    add_coprocessor(j, hw);
#endif /* !CONFIG_IDF_TARGET_LINUX */
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    cJSON_AddStringToObject(j, "app", app->project_name);
    cJSON_AddStringToObject(j, "version", app->version);
    cJSON_AddStringToObject(j, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(j, "chip", chip_model_str(chip.model));
    cJSON_AddNumberToObject(j, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(j, "cores", chip.cores);
    add_hardware(j, &chip);
    cJSON_AddNumberToObject(j, "uptime_s", (double)uptime_s());
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "min_free_heap", esp_get_minimum_free_heap_size());
    /* The internal pair, mirroring the whole-heap pair above: live, then low-water.
     * min_internal_free is the value the memoryTrough condition is raised from
     * (health.md), and before it existed the number behind that warning was readable
     * only in `last_reset`, AFTER a restart. free_internal is here so the pair can be
     * read without involving free_heap, which on a PSRAM build is MALLOC_CAP_DEFAULT
     * and counts PSRAM -- megabytes free there say nothing about internal RAM.
     *
     * Plain MALLOC_CAP_INTERNAL for both, deliberately NOT
     * esp_get_free_internal_heap_size() (which narrows to 8BIT|DMA): they have to be
     * the same cap set as each other and as espos_health's own sample, or the pair is
     * two different measurements -- the very mistake documented just below.
     *
     * Neither is comparable with the whole-heap pair: MALLOC_CAP_* are independent
     * flags, not a hierarchy, and heap_caps_get_minimum_free_size() SUMS each matching
     * region's own minimum rather than taking a minimum over one pool. */
    cJSON_AddNumberToObject(j, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j, "min_internal_free",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(j, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddBoolToObject(j, "config_storage_reset", espos_config_storage_was_reset());
    /* The merged tag, not the compiled one: a node that registers its settings
     * at run time changes the schema, and a client comparing this against the
     * ETag it was served would otherwise never refetch. */
    char schema_etag[ESPOS_CFG_ETAG_MAX];
    espos_config_schema_etag(schema_etag);
    cJSON_AddStringToObject(j, "schema_etag", schema_etag);
    cJSON_AddBoolToObject(j, "ui_storage", espos_httpd_static_mounted());
    cJSON_AddStringToObject(j, "ui", espos_httpd_ui_source());
    add_time(j);
    add_last_reset(j);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = espos_httpd_send_json(req, NULL, body);
    cJSON_free(body);
    return err;
}

/* Liveness for anyone on the network — a fleet page, a discovery tool: who
 * is this, which build, and does it want a key. Nothing here is a secret
 * that mDNS does not already advertise. */
static esp_err_t ping_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    cJSON_AddStringToObject(j, "app", app->project_name);
    cJSON_AddStringToObject(j, "version", app->version);
    cJSON_AddBoolToObject(j, "auth", espos_httpd_auth_required());
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = espos_httpd_send_json(req, NULL, body);
    cJSON_free(body);
    return err;
}

static volatile bool s_restart_pending;

bool espos_httpd_restart_pending(void)
{
    return s_restart_pending;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "restarting");
    esp_restart();
}

/* Reply first, restart 500 ms later so the response reaches the client. */
static esp_err_t schedule_restart(void)
{
    /* Generous stack: esp_restart runs the registered shutdown handlers. */
    if (s_restart_pending) {
        return ESP_OK;
    }
    const uint32_t stack = configMINIMAL_STACK_SIZE > 3072 ? configMINIMAL_STACK_SIZE * 4 : 3072;
    BaseType_t ok = xTaskCreate(restart_task, "espos_restart", stack, NULL, tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_restart_pending = true;
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    esp_err_t err = schedule_restart();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "restart_failed", esp_err_to_name(err));
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"rebooting\"}");
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "factory reset requested");
    /* Arm the restart first: PUTs are refused (503) from this point on, so
     * nothing can be written between the erase and the reboot. */
    esp_err_t err = schedule_restart();
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "restart_failed", esp_err_to_name(err));
    }
    err = espos_config_factory_reset();
    if (err != ESP_OK) {
        /* The store may be half-erased; the armed reboot still happens. */
        return espos_httpd_send_error(req, "500 Internal Server Error", "factory_reset_failed",
                                      "erase failed; rebooting anyway");
    }
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"factory_reset\",\"rebooting\":true}");
}

esp_err_t espos_httpd_register_system_api(void)
{
    static const struct {
        httpd_uri_t uri;
        uint32_t flags;
    } uris[] = {
        { { .uri = "/api/v1/system/ping", .method = HTTP_GET, .handler = ping_get }, ESPOS_HTTPD_PUBLIC },
        { { .uri = "/api/v1/system/info", .method = HTTP_GET, .handler = info_get }, ESPOS_HTTPD_PROTECTED },
        { { .uri = "/api/v1/system/reboot", .method = HTTP_POST, .handler = reboot_post }, ESPOS_HTTPD_PROTECTED },
        { { .uri = "/api/v1/system/factory-reset", .method = HTTP_POST, .handler = factory_reset_post }, ESPOS_HTTPD_PROTECTED },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register_ex(&uris[i].uri, uris[i].flags);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
