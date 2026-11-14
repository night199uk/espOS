/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/config and /api/v1/config/schema
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "espos_config.h"
#include "espos_config_desc.h"
#include "espos_httpd.h"
#include "espos_httpd_priv.h"

static const char *TAG = "espos_httpd";

static esp_err_t config_get(httpd_req_t *req)
{
    char ns[24] = { 0 };
    const char *only = NULL;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0) {
        char q[128];
        if (qlen >= sizeof(q)) {
            return espos_httpd_send_error(req, "414 URI Too Long", "uri_too_long", "query string too long");
        }
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            esp_err_t qerr = httpd_query_key_value(q, "ns", ns, sizeof(ns));
            if (qerr == ESP_OK) {
                only = ns;
            } else if (qerr != ESP_ERR_NOT_FOUND) {
                /* present but longer than any namespace can be */
                return espos_httpd_send_error(req, "404 Not Found", "unknown_namespace", "no such namespace");
            }
        }
    }
    char *json = NULL;
    esp_err_t err = espos_config_export_json(only, false, &json);
    if (err == ESP_ERR_NOT_FOUND) {
        return espos_httpd_send_error(req, "404 Not Found", "unknown_namespace", "no such namespace");
    }
    if (err != ESP_OK || !json) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "export_failed", esp_err_to_name(err));
    }
    err = espos_httpd_send_json(req, NULL, json);
    free(json);
    return err;
}

static esp_err_t config_put(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    if (espos_httpd_restart_pending()) {
        return espos_httpd_send_error(req, "503 Service Unavailable", "restarting", "device is restarting");
    }
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = espos_httpd_read_body(req, &body, &len);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    espos_config_import_result_t res;
    char *report = NULL;
    err = espos_config_import_json(body, len, false, &res, &report);
    free(body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config updated: %u key(s) changed%s", (unsigned)res.changed,
                 res.restart_required ? " (restart required)" : "");
        err = espos_httpd_send_json(req, NULL, report ? report : "{\"changed\":[],\"restart_required\":false}");
    } else if (err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "config rejected: %s: %s", res.error_path, res.error_msg);
        err = espos_httpd_send_json(req, "400 Bad Request",
                                    report ? report : "{\"error\":\"validation\",\"path\":\"\",\"message\":\"invalid\"}");
    } else {
        err = espos_httpd_send_error(req, "500 Internal Server Error", "write_failed", esp_err_to_name(err));
    }
    free(report);
    return err;
}

static esp_err_t schema_get(httpd_req_t *req)
{
    /* Not espos_cfg_schema_json any more: a graph node that registered its
     * own namespace has a section in the document too, and the ETag moves
     * with it so a browser holding the previous schema revalidates instead of
     * rendering a form without the node's settings. */
    char raw[ESPOS_CFG_ETAG_MAX];
    espos_config_schema_etag(raw);
    char etag[ESPOS_CFG_ETAG_MAX + 4];
    snprintf(etag, sizeof(etag), "\"%s\"", raw);
    char inm[sizeof(etag)];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }
    if (espos_config_runtime_ns_count() == 0) {
        /* No runtime namespace: the document is the compiled text, so send
         * it straight from flash. A heap copy needs one contiguous block the
         * size of the schema (tens of KB), which a classic ESP32 without
         * PSRAM often does not have. */
        httpd_resp_set_type(req, "application/schema+json");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, espos_cfg_schema_json, (ssize_t)espos_cfg_schema_json_len);
    }
    char *json = NULL;
    esp_err_t err = espos_config_schema_json(&json, NULL);
    if (err != ESP_OK || !json) {
        free(json);
        return espos_httpd_send_error(req, "500 Internal Server Error", "schema_failed",
                                      esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/schema+json");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

esp_err_t espos_httpd_register_config_api(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/config/schema", .method = HTTP_GET, .handler = schema_get },
        { .uri = "/api/v1/config", .method = HTTP_GET, .handler = config_get },
        { .uri = "/api/v1/config", .method = HTTP_PUT, .handler = config_put },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register_ex(&uris[i], ESPOS_HTTPD_PROTECTED);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
