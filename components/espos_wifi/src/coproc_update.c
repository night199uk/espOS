// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
/*
 * Updating the radio co-processor's firmware over the link it already runs
 * (ESP32-P4 host + ESP32-C6 over SDIO), so a boat never needs a USB cable
 * on the C6 to clear the coprocessorStale condition.
 *
 * Trust: the C6 image carries no signature this device can check, so the one
 * image accepted is the one whose SHA-256 this firmware was built with
 * (ESPOS_COPROC_FW_SHA256, coproc_image.h). This firmware is itself signed, so the
 * pin is as trustworthy as the firmware; the URL is only where to fetch it
 * from, and a mirror or a wrong file fails the hash, not the radio.
 *
 * Order: the whole image is downloaded into PSRAM and hashed BEFORE the first
 * byte reaches the co-processor. Streaming while hashing would be cheaper,
 * but a co-processor older than esp_hosted 2.6 switches its boot partition at
 * end(), with no separate activate, and a hash that only fails at the end
 * would then already be booting. The co-processor keeps two app slots, so an
 * interrupted write leaves the running image untouched.
 *
 * Applying it: the co-processor restarts itself into the new image a moment
 * after activate() (after end() on one older than 2.6, which has no activate),
 * and this host follows as soon as it notices: esp_hosted restarts it on the
 * transport failure that leaves behind, the hosted watchdog on
 * ESP_HOSTED_EVENT_CP_INIT, and a restart of our own if neither does. Its
 * start resets the co-processor again, so both chips start together on the
 * new pair.
 *
 * Not before the running firmware is confirmed: a restart while it is still
 * pending verification would roll this host back to the previous image, which
 * may expect a different co-processor than the one just installed.
 */

#include "sdkconfig.h"

#include "espos_wifi_priv.h"

#if defined(CONFIG_ESP_HOSTED) && defined(CONFIG_ESP_HOSTED_HOST_FEAT_OTA_READY)

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_ota.h"

#include "coproc_image.h"

#include "espos_httpd.h"

static const char *TAG = "espos_coproc";

/* The image and the host's esp_hosted are one pair; an esp_hosted bump that
 * misses coproc_image.h would offer an image the new host does not speak to. */
_Static_assert(ESPOS_COPROC_FW_VERSION_MAJOR == ESP_HOSTED_VERSION_MAJOR_1 &&
                   ESPOS_COPROC_FW_VERSION_MINOR == ESP_HOSTED_VERSION_MINOR_1 &&
                   ESPOS_COPROC_FW_VERSION_PATCH == ESP_HOSTED_VERSION_PATCH_1,
               "coproc_image.h pins a co-processor image for another esp_hosted than the host's");

/* espos_ota is optional and itself optionally requires espos_wifi, so these
 * are weak references rather than a dependency: NULL when it is not linked.
 * The quiesce pair parks real-time work (esp-sr's AFE on the cockpit) that
 * otherwise starves the idle task while an image downloads; see espos_ota.c. */
extern void espos_ota_quiesce_hook(void) __attribute__((weak));
extern void espos_ota_resume_hook(void) __attribute__((weak));
extern bool espos_ota_busy(void) __attribute__((weak));

/* Larger than any C6 app slot (1.75 MB in esp_hosted's 4 MB table), and a
 * bound on what a wrong URL can make this allocate. */
#define IMAGE_MAX (4 * 1024 * 1024)
#define URL_MAX   256
#define ERROR_MAX 96
/* Under the 3.x host's 1536-byte limit (EH_RPC_OTA_CHUNK_MAX), and what
 * esp_hosted's own OTA examples send, which older co-processors accept. */
#define CHUNK         1400
#define READ_CHUNK    4096
#define REDIRECTS_MAX 5
/* The co-processor restarts itself about 2 s after activate() and this host
 * follows it (see "Applying it" above); this is the fallback. */
#define RESTART_FALLBACK_MS 15000

typedef enum {
    CP_IDLE = 0,
    CP_DOWNLOADING,
    CP_VERIFYING,
    CP_WRITING,
    CP_RESTARTING,
    CP_FAILED,
} cp_state_t;

static const char *const STATE_NAMES[] = {
    [CP_IDLE] = "idle",
    [CP_DOWNLOADING] = "downloading",
    [CP_VERIFYING] = "verifying",
    [CP_WRITING] = "writing",
    [CP_RESTARTING] = "restarting",
    [CP_FAILED] = "failed",
};

typedef struct {
    cp_state_t state;
    size_t done;
    size_t total;
    char error[ERROR_MAX];
} cp_status_t;

static cp_status_t s_status;
static char s_url[URL_MAX];
/* True from start() until the update task has exited. Separate from the
 * state, which reads "failed" while the task is still closing its HTTP
 * client: a retry before then would run two downloads and two image buffers
 * at once. */
static bool s_running;
static bool s_quiesced;
/* The status is written by the update task and read by the httpd task; one
 * struct copy under a spinlock keeps a reader from seeing a new state beside
 * an old error. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_state(cp_state_t state, size_t done, size_t total)
{
    portENTER_CRITICAL(&s_mux);
    s_status.state = state;
    s_status.done = done;
    s_status.total = total;
    portEXIT_CRITICAL(&s_mux);
}

/* Formatted outside the lock: snprintf is not a call to make with interrupts
 * off. */
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    char msg[ERROR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_mux);
    s_status.state = CP_FAILED;
    memcpy(s_status.error, msg, sizeof(msg));
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "%s", msg);
}

/* The whole image into one PSRAM buffer; see "Order" above. */
static uint8_t *download(const char *url, size_t *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        /* Holds the request line, and a redirect target can be long: a GitHub
         * release download lands on a signed URL of about 1 KB, which the
         * default 512 bytes refuses. */
        .buffer_size_tx = 2048,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        fail("%s", "out of memory");
        return NULL;
    }
    uint8_t *buf = NULL;
    int64_t clen = 0;
    int status = 0;
    /* Followed by hand: open() + fetch_headers() does not, and a mirror behind
     * a proxy, or a GitHub download, answers with a redirect first. */
    for (int hop = 0; hop <= REDIRECTS_MAX; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            fail("could not connect: %s", esp_err_to_name(err));
            goto out;
        }
        clen = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400 || hop == REDIRECTS_MAX) {
            break;
        }
        esp_http_client_flush_response(c, NULL);
        esp_http_client_close(c);
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            break;
        }
    }
    if (status != 200) {
        fail("the server answered HTTP %d", status);
        goto out;
    }
    /* A length up front, not chunked: it sizes the one PSRAM buffer, and
     * every server this is pointed at (GitHub, the Signal K manager) sends
     * one. */
    if (clen <= 0 || clen > IMAGE_MAX) {
        fail("%s", "no usable Content-Length for the image");
        goto out;
    }
    buf = heap_caps_malloc((size_t)clen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fail("%s", "no PSRAM for the image");
        goto out;
    }
    size_t n = 0;
    set_state(CP_DOWNLOADING, 0, (size_t)clen);
    while (n < (size_t)clen) {
        /* Bounded reads: esp_http_client_read() fills the whole length it is
         * given before returning, which would leave no scheduling point and
         * no progress until the end. */
        size_t want = (size_t)clen - n < READ_CHUNK ? (size_t)clen - n : READ_CHUNK;
        int r = esp_http_client_read(c, (char *)buf + n, (int)want);
        if (r <= 0) {
            fail("%s", "the download stopped early");
            free(buf);
            buf = NULL;
            goto out;
        }
        n += (size_t)r;
        set_state(CP_DOWNLOADING, n, (size_t)clen);
        /* The same scheduling point espos_ota's download needs: without it
         * the hosted Rx drain task starves and SDIO drops packets. */
        vTaskDelay(1);
    }
    *len = n;
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

static bool hash_matches(const uint8_t *buf, size_t len)
{
    uint8_t sum[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, buf, len, sum) != 0) {
        return false;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    }
    if (strcasecmp(hex, ESPOS_COPROC_FW_SHA256) != 0) {
        ESP_LOGE(TAG, "image sha256 %s, this firmware accepts only %s", hex, ESPOS_COPROC_FW_SHA256);
        return false;
    }
    return true;
}

/* An esp_hosted older than 2.6 has no separate activate: end() switches the
 * boot slot and restarts the co-processor itself. A version that cannot be
 * read counts as new enough; "0.0.0", an image too old to announce one, does
 * not, as in esp_hosted's own host OTA example. A wrong guess is survivable
 * either way (see the activate() failure below). */
static bool cp_has_activate(void)
{
    espos_httpd_coproc_t cp;
    unsigned major = 0, minor = 0;
    if (!espos_httpd_coprocessor_hook(&cp) || sscanf(cp.version, "%u.%u", &major, &minor) != 2) {
        return true;
    }
    return major > 2 || (major == 2 && minor >= 6);
}

static void quiesce(bool on)
{
    if (on == s_quiesced) {
        return;
    }
    s_quiesced = on;
    void (*hook)(void) = on ? espos_ota_quiesce_hook : espos_ota_resume_hook;
    if (hook) {
        hook();
    }
}

static void finish(uint8_t *img)
{
    free(img);
    quiesce(false);
    espos_wifi_hosted_watchdog_hold(false);
    portENTER_CRITICAL(&s_mux);
    s_running = false;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}

static void update_task(void *arg)
{
    (void)arg;
    size_t len = 0;
    ESP_LOGI(TAG, "fetching co-processor firmware %s from %s", ESPOS_COPROC_FW_VERSION, s_url);
    quiesce(true);
    uint8_t *img = download(s_url, &len);
    if (!img) {
        finish(NULL);
        return;
    }
    set_state(CP_VERIFYING, len, len);
    if (!hash_matches(img, len)) {
        fail("%s", "the image is not the one this firmware was built for (sha256 mismatch)");
        finish(img);
        return;
    }

    /* begin() erases a 1.75 MB slot before it answers, and the writes then
     * keep the RPC channel busy for a while; neither is the wedge the
     * heartbeat watchdog exists for, and a restart in the middle would only
     * cost the update. */
    espos_wifi_hosted_watchdog_hold(true);
    set_state(CP_WRITING, 0, len);
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        fail("the co-processor refused to start an update: %s", esp_err_to_name(err));
        finish(img);
        return;
    }
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        err = esp_hosted_slave_ota_write(img + off, n);
        if (err != ESP_OK) {
            /* No end(): the half-written slot is never marked bootable, and
             * the next begin() erases it again. */
            fail("writing to the co-processor failed: %s", esp_err_to_name(err));
            finish(img);
            return;
        }
        set_state(CP_WRITING, off + n, len);
    }
    free(img);
    img = NULL;
    const bool activate = cp_has_activate();
    /* Before end(): on a co-processor without activate(), end() is the
     * switch, and the restart it triggers can arrive before this task runs
     * again. */
    if (!activate) {
        set_state(CP_RESTARTING, len, len);
    }
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        fail("the co-processor rejected the image: %s", esp_err_to_name(err));
        finish(NULL);
        return;
    }
    if (activate) {
        set_state(CP_RESTARTING, len, len);
        err = esp_hosted_slave_ota_activate();
        if (err != ESP_OK) {
            /* After a good end() a co-processor that predates activate() but
             * did not say so has already switched and is restarting; one that
             * has not keeps booting its old image. Either way the restart
             * below is the right next step, but nothing says the image is in:
             * the update reads failed, and the version after the restart
             * tells which image runs. s_running stays set until then. */
            fail("the co-processor did not activate the image: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "restarting to settle on whichever image the co-processor boots");
        }
    }
    quiesce(false);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "co-processor firmware %s installed; both chips restart to apply it",
                 ESPOS_COPROC_FW_VERSION);
    }
    vTaskDelay(pdMS_TO_TICKS(RESTART_FALLBACK_MS));
    ESP_LOGW(TAG, "the co-processor did not restart on its own; restarting");
    esp_restart();
}

/* A firmware still pending verification would be rolled back by the restart
 * that applies the co-processor image. */
static bool host_image_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static esp_err_t start(const char *url)
{
    const char *from = url && *url ? url : ESPOS_COPROC_FW_URL;
    if (strncmp(from, "http://", 7) != 0 && strncmp(from, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    /* No trust anchor to check an https server against, and an unverified
     * fetch is not one to make even with the hash behind it. */
    if (strncmp(from, "https://", 8) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (strlen(from) >= sizeof(s_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (host_image_pending()) {
        return ESP_ERR_INVALID_VERSION;
    }
    portENTER_CRITICAL(&s_mux);
    bool running = s_running;
    s_running = true;
    portEXIT_CRITICAL(&s_mux);
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Its restart at the end would cut a host update short, and the two
     * together hold two images in PSRAM. Checked after claiming s_running,
     * as espos_ota claims its state before checking ours, so two requests
     * racing each other cannot both miss. */
    if (espos_ota_busy && espos_ota_busy()) {
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NOT_FINISHED;
    }
    portENTER_CRITICAL(&s_mux);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CP_DOWNLOADING;
    portEXIT_CRITICAL(&s_mux);
    snprintf(s_url, sizeof(s_url), "%s", from);
    /* TLS plus esp_http_client want more than the default; the image itself
     * is in PSRAM, not on this stack. */
    if (xTaskCreate(update_task, "cp_update", 8192, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        s_status.state = CP_IDLE;
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static char *status_json(void)
{
    cp_status_t st;
    portENTER_CRITICAL(&s_mux);
    st = s_status;
    portEXIT_CRITICAL(&s_mux);
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return NULL;
    }
    cJSON_AddStringToObject(j, "state", STATE_NAMES[st.state]);
    if (st.total) {
        cJSON_AddNumberToObject(j, "done", (double)st.done);
        cJSON_AddNumberToObject(j, "total", (double)st.total);
    }
    if (st.state == CP_FAILED) {
        cJSON_AddStringToObject(j, "error", st.error);
    }
    /* What this firmware would install, so a client can tell whether an
     * update is worth offering without knowing espOS's esp_hosted pin. */
    cJSON *img = cJSON_AddObjectToObject(j, "image");
    if (img) {
        cJSON_AddStringToObject(img, "version", ESPOS_COPROC_FW_VERSION);
        cJSON_AddStringToObject(img, "sha256", ESPOS_COPROC_FW_SHA256);
        cJSON_AddStringToObject(img, "url", ESPOS_COPROC_FW_URL);
    }
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *j = status_json();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t r = espos_httpd_send_json(req, NULL, j);
    free(j);
    return r;
}

/* {} installs from the URL this firmware was built with; {"url": "..."} from
 * a mirror (the Signal K manager serves one). Either way the pinned hash
 * decides. */
static esp_err_t update_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = len ? cJSON_ParseWithLength(body, len) : cJSON_CreateObject();
    free(body);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "expected a JSON object");
    }
    const cJSON *url = cJSON_GetObjectItem(j, "url");
    if (url && !cJSON_IsString(url)) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "url must be a string");
    }
    esp_err_t err = start(url ? url->valuestring : NULL);
    cJSON_Delete(j);
    switch (err) {
    case ESP_OK:
        return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"updating\"}");
    case ESP_ERR_INVALID_STATE:
        return espos_httpd_send_error(req, "409 Conflict", "busy", "a co-processor update is already running");
    case ESP_ERR_NOT_FINISHED:
        return espos_httpd_send_error(req, "409 Conflict", "ota_busy",
                                      "a firmware update, check or rollback is in progress; retry after it");
    case ESP_ERR_INVALID_VERSION:
        return espos_httpd_send_error(req, "409 Conflict", "unconfirmed",
                                      "the running firmware is not confirmed yet; confirm it or wait, then retry");
    case ESP_ERR_INVALID_ARG:
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "expected an http(s) URL");
    case ESP_ERR_INVALID_SIZE:
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "url too long");
    case ESP_ERR_NOT_SUPPORTED:
        return espos_httpd_send_error(req, "400 Bad Request", "validation",
                                      "https needs a certificate bundle this firmware was built without; use http");
    default:
        return espos_httpd_send_error(req, "500 Internal Server Error", "internal", esp_err_to_name(err));
    }
}

bool espos_wifi_coproc_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool running = s_running;
    portEXIT_CRITICAL(&s_mux);
    return running;
}

esp_err_t espos_wifi_coproc_register_api(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/system/coprocessor/update", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/v1/system/coprocessor/update", .method = HTTP_POST, .handler = update_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

#else /* no hosted co-processor, or an esp_hosted without host-driven OTA */

/* No routes at all: a 404 is how a client learns this device has no
 * co-processor to update. */
esp_err_t espos_wifi_coproc_register_api(void) { return ESP_OK; }

bool espos_wifi_coproc_busy(void) { return false; }

#endif
