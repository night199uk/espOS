/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one HTTP client of espos_sk (esp_http_client, blocking). Three callers
 * share espos_sk_http_perform(): the access-request legs of the token
 * machine, the meta GET/PUT of the stream task's reconciliation, and the
 * public espos_sk_http_* API in sk_http_api.c.
 *
 *   POST /signalk/v1/access/requests {clientId, description, permissions}
 *        202 {state:"PENDING", href}      400 {message}   403 / 404 / 503
 *   GET  <href>   200 {state:"PENDING"|"COMPLETED", accessRequest:{permission, token}}
 *                 500 "Unable to check request: not found" when the server forgot it
 *   GET  /signalk/v1/api/self  (Bearer)   200 "vessels.<self>"   401 / 403
 *   GET/PUT /signalk/v1/api/vessels/self/<a/b/c>/meta   404 when unset; PUT takes {"value":{…}}
 *
 * Why a fresh handle and perform() every time -- both seen on the ESP32-P4:
 * open()/fetch_headers()/read() leaves cache_data_in_fetch_hdr set, and a
 * body arriving in the same segment as the headers (every small SignalK
 * reply) then trips assert(orig_raw_data == raw_data) in http_on_body;
 * reusing one handle across perform() calls desyncs the same two pointers.
 * A new handle per request with perform() enters neither path.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp: HTTP header names are case-insensitive */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "espos_net.h"
#include "espos_sk_http.h"
#include "espos_sk_priv.h"
#if CONFIG_ESPOS_SK_TLS
#include "espos_sk_tls.h"
#endif

static const char *TAG = "espos_sk";

/* The access-request documents and meta objects are small; the legs keep
 * the cap and timeout they always had. */
#define LEG_BODY_MAX   4096
#define LEG_TIMEOUT_MS 6000

/* A response body under construction. Grows as data arrives, so a 16 KiB
 * cap does not cost 16 KiB for a 200-byte reply; Content-Length is not
 * consulted on purpose, chunked replies have none. */
typedef struct {
    char *buf;
    size_t len, cap, max;
    bool truncated;
    bool oom;
    char *location;      /* where a Location header is copied, or NULL */
    size_t location_size;
} body_t;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (!b) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_HEADER && b->location && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        snprintf(b->location, b->location_size, "%s", evt->header_value);
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || b->truncated || b->oom) {
        return ESP_OK;
    }
    size_t n = (size_t)evt->data_len;
    if (n > b->max - b->len) {
        /* Keep what fits and flag it; keep draining so the connection closes
         * cleanly. The caller sees truncated and must not parse the body. */
        n = b->max - b->len;
        b->truncated = true;
    }
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 512;
        while (want < b->len + n + 1) {
            want *= 2;
        }
        if (want > b->max + 1) {
            want = b->max + 1;
        }
        char *grown = realloc(b->buf, want);
        if (!grown) {
            b->oom = true;
            return ESP_OK;
        }
        b->buf = grown;
        b->cap = want;
    }
    memcpy(b->buf + b->len, evt->data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return ESP_OK;
}

/* Requests in flight are bounded (Kconfig ESPOS_SK_HTTP_MAX_CONCURRENT):
 * each open one is a socket plus, over TLS, ~20 KB of RAM, and a display
 * refreshing a layout would otherwise open one per widget at once. Created
 * on first use because espos_sk has no init hook guaranteed to run before an
 * application task calls in; two first callers race, so the loser of the
 * exchange deletes its copy and uses the winner's. */
static _Atomic(SemaphoreHandle_t) s_slots;

static SemaphoreHandle_t slots(void)
{
    SemaphoreHandle_t sem = atomic_load(&s_slots);
    if (sem) {
        return sem;
    }
    SemaphoreHandle_t fresh = xSemaphoreCreateCounting(CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT, CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT);
    if (!fresh) {
        return NULL;
    }
    SemaphoreHandle_t expected = NULL;
    if (atomic_compare_exchange_strong(&s_slots, &expected, fresh)) {
        return fresh;
    }
    vSemaphoreDelete(fresh);
    return expected;
}

static const char *method_name(espos_sk_http_method_t m)
{
    switch (m) {
    case ESPOS_SK_HTTP_PUT: return "PUT";
    case ESPOS_SK_HTTP_POST: return "POST";
    case ESPOS_SK_HTTP_DELETE: return "DELETE";
    default: return "GET";
    }
}

static esp_http_client_method_t idf_method(espos_sk_http_method_t m)
{
    switch (m) {
    case ESPOS_SK_HTTP_PUT: return HTTP_METHOD_PUT;
    case ESPOS_SK_HTTP_POST: return HTTP_METHOD_POST;
    case ESPOS_SK_HTTP_DELETE: return HTTP_METHOD_DELETE;
    default: return HTTP_METHOD_GET;
    }
}

esp_err_t espos_sk_http_build_url(const espos_sk_server_t *srv, const char *scheme, const char *path, char *out, size_t n)
{
    int w = snprintf(out, n, "%s://%s:%u%s%s", scheme, srv->host, (unsigned)srv->port, path[0] == '/' ? "" : "/", path);
    if (w < 0 || (size_t)w >= n) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t espos_sk_http_self_path(const char *sk_path, const char *suffix, char *out, size_t n)
{
    /* dots become slashes: environment.wind.speedApparent → environment/wind/speedApparent */
    int w0 = snprintf(out, n, "/signalk/v1/api/vessels/self/");
    if (w0 < 0 || (size_t)w0 >= n) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    size_t o = (size_t)w0;
    for (; *sk_path && o + 1 < n; sk_path++) {
        out[o++] = *sk_path == '.' ? '/' : *sk_path;
    }
    if (*sk_path) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    int w = snprintf(out + o, n - o, "%s", suffix);
    if (w < 0 || (size_t)w >= n - o) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t espos_sk_http_perform(const espos_sk_http_req_t *rq, espos_sk_http_resp_t *r)
{
    memset(r, 0, sizeof(*r));
    char url[ESPOS_SK_URL_MAX];
    esp_err_t err = espos_sk_http_build_url(rq->srv, rq->srv->tls ? "https" : "http", rq->path, url, sizeof(url));
    if (err != ESP_OK) {
        r->err = err;
        return err;
    }
    SemaphoreHandle_t sem = slots();
    if (!sem) {
        r->err = ESP_ERR_NO_MEM;
        return r->err;
    }
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(rq->timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "%s %s: %d requests already in flight, none finished within %u ms", method_name(rq->method), url,
                 CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT, (unsigned)rq->timeout_ms);
        r->err = ESP_ERR_TIMEOUT;
        return r->err;
    }
    body_t b = { .max = rq->max_body, .location = rq->capture_location, .location_size = rq->capture_location_size };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = idf_method(rq->method),
        .timeout_ms = (int)rq->timeout_ms,
        .event_handler = on_event,
        .user_data = &b,
        .disable_auto_redirect = true,
        .user_agent = espos_net_user_agent(),
        .keep_alive_enable = false,
        .buffer_size_tx = 1536, /* room for "Authorization: Bearer <jwt up to 1 KiB>" */
#if CONFIG_ESPOS_SK_TLS
        /* Not esp_crt_bundle_attach: espos_sk_tls_attach() is the trust store
         * (sk_tls.c) and falls back to the bundle only in bundle mode. The
         * common-name check is mbedTLS's, against a certificate we have
         * already decided about by fingerprint and SAN set — and a boat server
         * is reached by IP as often as by name, which that check fails. So it
         * is off wherever the trust store is doing the deciding. */
        .crt_bundle_attach = rq->srv->tls ? espos_sk_tls_attach : NULL,
        .skip_cert_common_name_check = rq->srv->tls && espos_sk_tls_trust_mode() != ESPOS_SK_TLS_TRUST_BUNDLE,
#endif
    };
#if CONFIG_ESPOS_SK_TLS
    /* One handshake at a time, device-wide, with a pre-flight memory check.
     * Held across perform() because that is where the handshake happens; the
     * body read after it is short and this is not a hot path. */
    if (rq->srv->tls) {
        esp_err_t hs = espos_sk_tls_handshake_begin(CONFIG_ESPOS_SK_TLS_HANDSHAKE_TIMEOUT_MS);
        if (hs != ESP_OK) {
            xSemaphoreGive(sem);
            free(b.buf);
            ESP_LOGW(TAG, "%s %s: deferred (%s)", method_name(rq->method), url, esp_err_to_name(hs));
            r->err = hs;
            return hs;
        }
    }
#endif
    int status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        err = ESP_ERR_NO_MEM;
    } else {
        const char *accept = rq->accept ? rq->accept : "application/json";
        if (accept[0]) {
            esp_http_client_set_header(c, "Accept", accept);
        }
        if (rq->json_body) {
            esp_http_client_set_header(c, "Content-Type", "application/json");
            esp_http_client_set_post_field(c, rq->json_body, (int)strlen(rq->json_body));
        }
        char auth[ESPOS_SK_TOKEN_CAP + 8];
        if (rq->bearer && rq->bearer[0]) {
            snprintf(auth, sizeof(auth), "Bearer %s", rq->bearer);
            esp_http_client_set_header(c, "Authorization", auth);
        }
        err = esp_http_client_perform(c);
        status = esp_http_client_get_status_code(c);
        esp_http_client_cleanup(c);
        if (err != ESP_OK && (status == 401 || status == 403)) {
            /* esp_http_client treats 401 as "needs auth" and fails perform()
             * before reading the body; the status is still there and, for us,
             * a perfectly good answer (token rejected / not enabled). */
            err = ESP_OK;
        }
        if (err == ESP_OK && b.oom) {
            err = ESP_ERR_NO_MEM;
        }
    }
#if CONFIG_ESPOS_SK_TLS
    if (rq->srv->tls) {
        /* Commit only on an answer the server itself must have produced. A
         * completed handshake is not enough: anything can complete one with a
         * certificate of its own, and it is exactly that certificate we must
         * not adopt. A status from the far end means we are talking to
         * something that speaks HTTP over this connection, which is as much
         * as one request can establish; the token machine's "is this a
         * SignalK server" probe does the rest.
         *
         * Outside the `if (c)` above deliberately: a handle that failed to
         * allocate still holds the slot this took. */
        if (status > 0) {
            espos_sk_tls_commit(rq->srv->self[0] ? rq->srv->self : NULL);
        } else {
            espos_sk_tls_discard();
        }
        espos_sk_tls_handshake_end();
        /* A handshake that produced no status at all, with the trust store
         * saying why: hand the caller a cert_error rather than the
         * indistinguishable "unreachable". */
        if (status == 0 && espos_sk_tls_last_error()[0]) {
            snprintf(r->cert_reason, sizeof(r->cert_reason), "%s", espos_sk_tls_last_error());
            r->cert_error = true;
        }
    }
#endif
    xSemaphoreGive(sem);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", method_name(rq->method), url, esp_err_to_name(err));
        free(b.buf);
        r->err = err;
        return err;
    }
    if (!b.buf) {
        /* an empty reply still hands back a string */
        b.buf = calloc(1, 1);
        if (!b.buf) {
            r->err = ESP_ERR_NO_MEM;
            return r->err;
        }
    }
    r->status = status;
    r->body = b.buf;
    r->len = b.len;
    r->truncated = b.truncated;
    return ESP_OK;
}

/* The legs' view of a request: an HTTP status (0 = transport failure) and
 * the body text in *body_out (malloc'ed, NUL-terminated; NULL on failure). */
static int leg_ex(const espos_sk_server_t *srv, espos_sk_http_method_t method, const char *path, const char *json_body,
                  const char *bearer, char **body_out, espos_sk_http_result_t *out, bool *truncated)
{
    espos_sk_http_req_t rq = {
        .srv = srv,
        .method = method,
        .path = path,
        .json_body = json_body,
        .bearer = bearer,
        .timeout_ms = LEG_TIMEOUT_MS,
        .max_body = LEG_BODY_MAX,
    };
    espos_sk_http_resp_t r;
    *body_out = NULL;
    esp_err_t err = espos_sk_http_perform(&rq, &r);
    /* A certificate refusal travels even when the call failed: it is the one
     * transport failure the machine must not treat as "server unreachable".
     * `out` is NULL for the legs that have nowhere to put it (the meta calls). */
    if (out && r.cert_error) {
        out->cert_error = true;
        snprintf(out->cert_reason, sizeof(out->cert_reason), "%s", r.cert_reason);
    }
    if (err != ESP_OK) {
        return 0;
    }
    *body_out = r.body; /* ownership moves to the caller */
    if (truncated) {
        *truncated = r.truncated;
    }
    return r.status;
}

static int leg(const espos_sk_server_t *srv, espos_sk_http_method_t method, const char *path, const char *json_body,
               const char *bearer, char **body_out, espos_sk_http_result_t *out)
{
    return leg_ex(srv, method, path, json_body, bearer, body_out, out, NULL);
}

static void copy_str(char *dst, size_t size, const cJSON *j)
{
    if (cJSON_IsString(j) && j->valuestring) {
        snprintf(dst, size, "%s", j->valuestring);
    } else {
        dst[0] = '\0';
    }
}

/* Parse a request/poll reply document into the result. */
static void parse_reply(const char *body, espos_sk_http_result_t *out)
{
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        /* plain-text bodies ("Unable to check request: not found") */
        snprintf(out->message, sizeof(out->message), "%s", body);
        return;
    }
    copy_str(out->state, sizeof(out->state), cJSON_GetObjectItem(j, "state"));
    copy_str(out->href, sizeof(out->href), cJSON_GetObjectItem(j, "href"));
    copy_str(out->message, sizeof(out->message), cJSON_GetObjectItem(j, "message"));
    cJSON *ar = cJSON_GetObjectItem(j, "accessRequest");
    if (cJSON_IsObject(ar)) {
        copy_str(out->permission, sizeof(out->permission), cJSON_GetObjectItem(ar, "permission"));
        const cJSON *tok = cJSON_GetObjectItem(ar, "token");
        if (cJSON_IsString(tok) && tok->valuestring && strlen(tok->valuestring) >= sizeof(out->token)) {
            /* A clipped bearer is refused by the server, or worse, accepted
             * nowhere and kept: report it as a rejection with a reason. */
            out->token[0] = '\0';
            snprintf(out->message, sizeof(out->message), "%s", ESPOS_SK_TOKEN_TOO_LONG);
        } else {
            copy_str(out->token, sizeof(out->token), tok);
        }
    }
    /* a COMPLETED reply may carry the failing statusCode inside the body */
    cJSON *sc = cJSON_GetObjectItem(j, "statusCode");
    if (cJSON_IsNumber(sc) && strcmp(out->state, "COMPLETED") == 0 && sc->valueint >= 400 && out->permission[0] == '\0') {
        if (!out->message[0]) {
            snprintf(out->message, sizeof(out->message), "request rejected (%d)", sc->valueint);
        }
    }
    cJSON_Delete(j);
}

void espos_sk_http_request(const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "clientId", cfg->client_id);
    cJSON_AddStringToObject(j, "description", cfg->description);
    cJSON_AddStringToObject(j, "permissions", cfg->permissions);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return;
    }
    char *resp = NULL;
    out->http_status = leg(srv, ESPOS_SK_HTTP_POST, "/signalk/v1/access/requests", body, NULL, &resp, out);
    free(body);
    if (resp) {
        parse_reply(resp, out);
        free(resp);
    }
    if (out->http_status == 404) {
        /* 404 means "security disabled" only on a SignalK server; a random
         * HTTP host says 404 too. GET /signalk tells them apart. */
        char *probe = NULL;
        int st = leg(srv, ESPOS_SK_HTTP_GET, "/signalk", NULL, NULL, &probe, out);
        bool is_sk = st == 200 && probe && strstr(probe, "endpoints");
        free(probe);
        if (!is_sk) {
            out->http_status = 599;
        }
    }
    ESP_LOGI(TAG, "access request → %d %s %s", out->http_status, out->state, out->href[0] ? out->href : out->message);
}

void espos_sk_http_poll(const espos_sk_server_t *srv, const char *href, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    out->http_status = leg(srv, ESPOS_SK_HTTP_GET, href, NULL, NULL, &resp, out);
    if (resp) {
        parse_reply(resp, out);
        free(resp);
    }
    ESP_LOGD(TAG, "poll → %d %s %s", out->http_status, out->state, out->permission);
}

void espos_sk_http_verify(const espos_sk_server_t *srv, const char *token, espos_sk_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    out->http_status = leg(srv, ESPOS_SK_HTTP_GET, "/signalk/v1/api/self", NULL, token, &resp, out);
    if (resp && out->http_status == 200) {
        /* body is a JSON string: "vessels.urn:mrn:..." */
        cJSON *j = cJSON_Parse(resp);
        if (cJSON_IsString(j) && j->valuestring) {
            const char *v = j->valuestring;
            if (strncmp(v, "vessels.", 8) == 0) {
                v += 8;
            }
            snprintf(out->self, sizeof(out->self), "%s", v);
        }
        cJSON_Delete(j);
    }
    free(resp);
    ESP_LOGD(TAG, "verify → %d %s", out->http_status, out->self);
}

/* ------------------------------------------------------ scheme probe */

/* Does this host want https? One unauthenticated GET of http://host:port/signalk
 * with redirects off. Three outcomes matter:
 *
 *   30x + Location: https://…   the server redirects plaintext away, which is
 *                               what signalk-server with ssl:true does; the
 *                               Location may also name a different port.
 *   connection refused          nothing listens on the plain port -- try https
 *                               on the same port once before giving up, since a
 *                               server that only speaks TLS is the other common
 *                               shape.
 *   anything else               plain http works; use it.
 *
 * And a fourth that is not an answer: nothing responds on either scheme.
 * That is a host that is down, still booting or not reachable yet, and it
 * says nothing about what the server speaks -- so it is reported as
 * NO_ANSWER, never as plain.
 *
 * Without a token, deliberately (SensESP #1057): a probe is aimed at a host
 * that has not been established as our server yet, and the token must not be
 * handed to whatever answered — the whole point of the exercise is that we do
 * not know what that is.
 *
 * Runs once per server selection, not per request; an answer is cached in the
 * server entry.
 */
espos_sk_probe_t espos_sk_http_probe_https(const char *host, uint16_t port, uint16_t *out_port)
{
    espos_sk_server_t plain = { .port = port, .tls = false };
    snprintf(plain.host, sizeof(plain.host), "%s", host);
    if (out_port) {
        *out_port = port;
    }
    char loc[ESPOS_SK_URL_MAX] = { 0 };
    espos_sk_http_req_t rq = {
        .srv = &plain,
        .method = ESPOS_SK_HTTP_GET,
        .path = "/signalk",
        .accept = "",
        .timeout_ms = LEG_TIMEOUT_MS,
        .max_body = 512,
        .capture_location = loc,
        .capture_location_size = sizeof(loc),
    };
    espos_sk_http_resp_t r;
    esp_err_t err = espos_sk_http_perform(&rq, &r);
    int status = r.status;
    espos_sk_http_resp_free(&r);
    if (err == ESP_OK && status >= 300 && status < 400 && strncmp(loc, "https://", 8) == 0) {
        /* Take the port out of the Location when it names one: signalk-server
         * with ssl:true commonly listens for TLS somewhere other than the
         * plain port, and following the redirect's host:port is the only way
         * to land on it. */
        const char *p = strchr(loc + 8, ':');
        const char *slash = strchr(loc + 8, '/');
        if (p && (!slash || p < slash) && out_port) {
            int v = atoi(p + 1);
            if (v > 0 && v <= 65535) {
                *out_port = (uint16_t)v;
            }
        }
        ESP_LOGI(TAG, "%s:%u redirects to https (%s)", host, (unsigned)port, loc);
        return ESPOS_SK_PROBE_TLS;
    }
    if (err == ESP_OK || status > 0) {
        return ESPOS_SK_PROBE_PLAIN;
    }
    /* Nothing answered on the plain port. A TLS-only server is the other
     * thing that looks like this, so ask once before concluding the host
     * is simply down -- and let the trust store judge the certificate,
     * which is what tells the two apart. */
    espos_sk_server_t secure = plain;
    secure.tls = true;
    espos_sk_http_req_t rq2 = rq;
    rq2.srv = &secure;
    rq2.capture_location = NULL;
    rq2.capture_location_size = 0;
    espos_sk_http_resp_t r2;
    esp_err_t e2 = espos_sk_http_perform(&rq2, &r2);
    int st2 = r2.status;
    espos_sk_http_resp_free(&r2);
    if (e2 == ESP_OK && st2 > 0) {
        ESP_LOGI(TAG, "%s:%u answers only over https", host, (unsigned)port);
        return ESPOS_SK_PROBE_TLS;
    }
    return ESPOS_SK_PROBE_NO_ANSWER;
}

/* -------------------------------------------------------------- meta */

int espos_sk_http_get_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *ours,
                           espos_sk_meta_seen_t *seen)
{
    char url[192];
    *seen = ESPOS_SK_META_NONE;
    if (espos_sk_http_self_path(path, "/meta", url, sizeof(url)) != ESP_OK) {
        return 0;
    }
    char *resp = NULL;
    bool truncated = false;
    int status = leg_ex(srv, ESPOS_SK_HTTP_GET, url, NULL, token, &resp, NULL, &truncated);
    /* A cut-off or unparsable reply is a server that has meta, more than the
     * device can read -- not one that has none. An empty body has none. */
    if (status == 200 && resp && resp[0]) {
        cJSON *j = truncated ? NULL : cJSON_Parse(resp);
        if (!j) {
            *seen = ESPOS_SK_META_UNREADABLE;
        } else if (cJSON_IsObject(j) && j->child) {
            *seen = ours && espos_sk_meta_holds(resp, ours) ? ESPOS_SK_META_OURS : ESPOS_SK_META_PRESENT;
        }
        cJSON_Delete(j);
    }
    free(resp);
    return status;
}

int espos_sk_http_put_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *meta_json)
{
    char url[192];
    if (espos_sk_http_self_path(path, "/meta", url, sizeof(url)) != ESP_OK) {
        return 0;
    }
    size_t n = strlen(meta_json) + 16;
    char *body = malloc(n);
    if (!body) {
        return 0;
    }
    snprintf(body, n, "{\"value\":%s}", meta_json);
    char *resp = NULL;
    int status = leg(srv, ESPOS_SK_HTTP_PUT, url, body, token, &resp, NULL);
    free(body);
    free(resp);
    return status;
}
