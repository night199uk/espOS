/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/ota — status, check, install, confirm, rollback.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "espos_httpd.h"
#include "espos_httpd_sse.h"
#include "espos_ota.h"

static esp_err_t status_get(httpd_req_t *req)
{
    char *j = espos_ota_status_json();
    if (!j) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t r = espos_httpd_send_json(req, NULL, j);
    free(j);
    return r;
}

static esp_err_t reply(httpd_req_t *req, esp_err_t err, const char *word)
{
    if (err == ESP_ERR_INVALID_STATE) {
        return espos_httpd_send_error(req, "409 Conflict", "busy", "an update or check is in progress");
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        /* Its own code: a client that adopts a running update on "busy"
         * would otherwise take this for one. */
        return espos_httpd_send_error(req, "409 Conflict", "coprocessor_busy",
                                      "a radio co-processor update is running; retry after it");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return espos_httpd_send_error(req, "404 Not Found", "not_found", "no update known; check first");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return espos_httpd_send_error(req, "400 Bad Request", "validation", "expected an http(s) URL");
    }
    if (err != ESP_OK) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "internal", esp_err_to_name(err));
    }
    char body[48];
    snprintf(body, sizeof(body), "{\"status\":\"%s\"}", word);
    return espos_httpd_send_json(req, "202 Accepted", body);
}

static esp_err_t check_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    return reply(req, espos_ota_check_now(), "checking");
}

/* {"url": "http://…/espos.bin"} → install from URL; {} → install the available build */
static esp_err_t install_post(httpd_req_t *req)
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
    esp_err_t err;
    if (cJSON_IsString(url) && url->valuestring[0]) {
        err = espos_ota_install_url(url->valuestring);
    } else {
        err = espos_ota_install_available();
    }
    cJSON_Delete(j);
    return reply(req, err, "installing");
}

static esp_err_t confirm_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    return reply(req, espos_ota_confirm(), "confirmed");
}

static esp_err_t rollback_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    return reply(req, espos_ota_rollback(), "rolling_back");
}

static void sse_hello(int client, void *arg)
{
    (void)arg;
    char *j = espos_ota_status_json();
    if (j) {
        espos_httpd_sse_send(client, "ota", j);
        free(j);
    }
}

esp_err_t espos_ota_register_api(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/ota/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/v1/ota/check", .method = HTTP_POST, .handler = check_post },
        { .uri = "/api/v1/ota", .method = HTTP_POST, .handler = install_post },
        { .uri = "/api/v1/ota/confirm", .method = HTTP_POST, .handler = confirm_post },
        { .uri = "/api/v1/ota/rollback", .method = HTTP_POST, .handler = rollback_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return espos_httpd_sse_on_connect(sse_hello, NULL);
}
