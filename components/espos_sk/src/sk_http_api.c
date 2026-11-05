/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The public HTTP helpers of espos_sk_http.h: resolve the selected server
 * and the current token, hand the request to the shared core in sk_http.c,
 * and turn a 401/403 into a report to the token machine. Nothing here
 * touches esp_http_client directly -- that is the point of the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "espos_sk.h"
#include "espos_sk_http.h"
#include "espos_sk_priv.h"

static const char *TAG = "espos_sk";

#define DEFAULT_TIMEOUT_MS 6000
#define DEFAULT_MAX_BODY   16384

static esp_err_t request(espos_sk_http_method_t method, const char *path, const char *json, const espos_sk_http_opts_t *o,
                         espos_sk_http_resp_t *r)
{
    if (!r) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(r, 0, sizeof(*r));
    if (!path) {
        r->err = ESP_ERR_INVALID_ARG;
        return r->err;
    }
    espos_sk_http_opts_t opts = o ? *o : (espos_sk_http_opts_t) { 0 };
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK) {
        r->err = ESP_ERR_INVALID_STATE;
        return r->err;
    }
    /* A snapshot, not a pointer into espos_sk: the token may be replaced
     * while this request is on the wire. Heap, not stack: 1 KiB is a lot to
     * ask of an application task that just wants one value. */
    char *token = NULL;
    if (!opts.no_auth) {
        token = malloc(ESPOS_SK_TOKEN_CAP);
        if (!token) {
            r->err = ESP_ERR_NO_MEM;
            return r->err;
        }
        if (espos_sk_get_token(token, ESPOS_SK_TOKEN_CAP) != ESP_OK) {
            token[0] = '\0';
        }
    }
    espos_sk_http_req_t rq = {
        .srv = &srv,
        .method = method,
        .path = path,
        .json_body = json,
        .bearer = token,
        .accept = opts.accept,
        .timeout_ms = opts.timeout_ms ? opts.timeout_ms : DEFAULT_TIMEOUT_MS,
        .max_body = opts.max_body ? opts.max_body : DEFAULT_MAX_BODY,
    };
    esp_err_t err = espos_sk_http_perform(&rq, r);
    bool sent_token = token && token[0];
    free(token);
    if (err == ESP_OK && (r->status == 401 || r->status == 403) && sent_token && !opts.no_report_unauthorized) {
        /* Only when our token was on the request: a 401 without one means the
         * server wants a token the machine is already busy obtaining. The
         * report is a queued command, so it cannot wait on this task. */
        ESP_LOGW(TAG, "%s: HTTP %d with our token, reporting to the token machine", path, r->status);
        espos_sk_report_unauthorized();
    }
    return err;
}

esp_err_t espos_sk_http_get(const char *path, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r)
{
    return request(ESPOS_SK_HTTP_GET, path, NULL, o, r);
}

esp_err_t espos_sk_http_put(const char *path, const char *json, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r)
{
    return request(ESPOS_SK_HTTP_PUT, path, json, o, r);
}

esp_err_t espos_sk_http_post(const char *path, const char *json, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r)
{
    return request(ESPOS_SK_HTTP_POST, path, json, o, r);
}

esp_err_t espos_sk_http_delete(const char *path, const espos_sk_http_opts_t *o, espos_sk_http_resp_t *r)
{
    return request(ESPOS_SK_HTTP_DELETE, path, NULL, o, r);
}

void espos_sk_http_resp_free(espos_sk_http_resp_t *r)
{
    if (!r) {
        return;
    }
    free(r->body);
    memset(r, 0, sizeof(*r));
}

/* ---------------------------------------------------------------- urls */

static esp_err_t url_for(const char *path, const char *plain, const char *secure, char *out, size_t n)
{
    if (!path || !out || !n) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return espos_sk_http_build_url(&srv, srv.tls ? secure : plain, path, out, n);
}

esp_err_t espos_sk_url(const char *path, char *out, size_t n)
{
    return url_for(path, "http", "https", out, n);
}

esp_err_t espos_sk_ws_url(const char *path, char *out, size_t n)
{
    return url_for(path, "ws", "wss", out, n);
}

/* ------------------------------------------------------- value / meta */

/* GET a vessels.self node and hand back one member of it as JSON text, or
 * the whole document when `member` is NULL and it is an object. */
static esp_err_t fetch_json(const char *sk_path, const char *suffix, const char *member, char **out_json)
{
    if (!sk_path || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    char path[ESPOS_SK_URL_MAX];
    esp_err_t err = espos_sk_http_self_path(sk_path, suffix, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    espos_sk_http_resp_t r;
    err = espos_sk_http_get(path, NULL, &r);
    if (err != ESP_OK) {
        return err;
    }
    if (r.status == 404) {
        err = ESP_ERR_NOT_FOUND;
    } else if (r.status == 401 || r.status == 403) {
        err = ESP_ERR_NOT_ALLOWED;
    } else if (r.status != 200) {
        ESP_LOGW(TAG, "GET %s: HTTP %d", path, r.status);
        err = ESP_FAIL;
    } else if (r.truncated) {
        ESP_LOGW(TAG, "GET %s: reply over %u bytes, not parsed", path, (unsigned)r.len);
        err = ESP_ERR_INVALID_SIZE;
    } else {
        cJSON *j = cJSON_Parse(r.body);
        if (!j) {
            /* A 200 that is not JSON: a server does that for a path without
             * meta, so for meta it means "none" rather than "broken". */
            err = member ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_FOUND;
        } else {
            cJSON *v = member ? cJSON_GetObjectItem(j, member) : (cJSON_IsObject(j) ? j : NULL);
            if (!v) {
                err = ESP_ERR_NOT_FOUND;
            } else {
                *out_json = cJSON_PrintUnformatted(v);
                if (!*out_json) {
                    err = ESP_ERR_NO_MEM;
                }
            }
            cJSON_Delete(j);
        }
    }
    espos_sk_http_resp_free(&r);
    return err;
}

esp_err_t espos_sk_get_value(const char *sk_path, char **out_json)
{
    return fetch_json(sk_path, "", "value", out_json);
}

esp_err_t espos_sk_get_meta(const char *sk_path, char **out_json)
{
    return fetch_json(sk_path, "/meta", NULL, out_json);
}
