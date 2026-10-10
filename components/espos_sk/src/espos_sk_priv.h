/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_sk.h"
#include "espos_sk_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent device state (NVS namespace "skstate"): sk_store.c */
esp_err_t espos_sk_store_load(espos_sk_tok_store_t *out, char client_id[40]);
esp_err_t espos_sk_store_save(const espos_sk_tok_store_t *st);

/* Server choice (sk_select.c, pure). The anchor is the server the stored
 * token -- or, without one, the pending request -- was issued by. Its fields
 * point into the store and are "" / 0 when unknown. */
typedef struct {
    const char *host;
    uint16_t port;
    const char *self;
} espos_sk_anchor_t;

void espos_sk_store_anchor(const espos_sk_tok_store_t *st, espos_sk_anchor_t *out);
/* The anchored entry of list[0..n): the one at the anchor's address (with a
 * self URN that does not contradict the anchor's), else --
 * when the anchor has no address, or self_fallback says it has been gone long
 * enough to have moved -- among those with its self URN the current server,
 * else the lowest address. -1 when none matches. */
int espos_sk_find_anchored(const espos_sk_discovered_t *list, size_t n, const espos_sk_anchor_t *anchor,
                           const char *cur_host, uint16_t cur_port, bool self_fallback);
/* d is at host:port. An empty host matches nothing. */
bool espos_sk_same_addr(const espos_sk_discovered_t *d, const char *host, uint16_t port);
/* d is the anchored server: at its address, with a self URN that does not
 * contradict the anchor's. */
bool espos_sk_is_anchor(const espos_sk_discovered_t *d, const espos_sk_anchor_t *anchor);
/* Election order: self URN, then the current server, then the address. */
bool espos_sk_disc_before(const espos_sk_discovered_t *a, const espos_sk_discovered_t *b,
                          const char *cur_host, uint16_t cur_port);

/* HTTP (blocking, esp_http_client): sk_http.c */
void espos_sk_http_request(const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg, espos_sk_http_result_t *out);
void espos_sk_http_poll(const espos_sk_server_t *srv, const char *href, espos_sk_http_result_t *out);
void espos_sk_http_verify(const espos_sk_server_t *srv, const char *token, espos_sk_http_result_t *out);

/* The shared HTTP core (sk_http.c): one esp_http_client per call, perform()
 * only -- see espos_sk_http.h for why. srv, path and bearer are the caller's:
 * the token machine and the stream task pass what they were handed, the
 * public API (sk_http_api.c) snapshots the selected server and the current
 * token. timeout_ms and max_body must be set. Returns ESP_OK when a status
 * arrived (any status); r->body is then the caller's to free. */
typedef enum {
    ESPOS_SK_HTTP_GET = 0,
    ESPOS_SK_HTTP_PUT = 1,
    ESPOS_SK_HTTP_POST = 2,
    ESPOS_SK_HTTP_DELETE = 3,
} espos_sk_http_method_t;

typedef struct {
    const espos_sk_server_t *srv;
    espos_sk_http_method_t method;
    const char *path;        /* absolute: "/signalk/..." */
    const char *json_body;   /* NULL = no body */
    const char *bearer;      /* NULL or "" = no Authorization header */
    const char *accept;      /* NULL = application/json, "" = none */
    uint32_t timeout_ms;
    size_t max_body;
    /* Optional: copy the Location response header here. Redirects are never
     * followed (disable_auto_redirect), so this is the only way to see where
     * a 30x wanted to send us -- which is exactly what the scheme probe is
     * asking. NULL = do not capture. */
    char *capture_location;
    size_t capture_location_size;
} espos_sk_http_req_t;

esp_err_t espos_sk_http_perform(const espos_sk_http_req_t *rq, espos_sk_http_resp_t *r);
/* What the scheme probe learned. NO_ANSWER is not "plain http": nothing
 * answered on either scheme -- no network yet, the host down or still
 * booting, a certificate the trust store refused -- so it is not an answer to
 * remember. */
typedef enum {
    ESPOS_SK_PROBE_NO_ANSWER = 0,
    ESPOS_SK_PROBE_PLAIN = 1,
    ESPOS_SK_PROBE_TLS = 2,
} espos_sk_probe_t;

/**
 * sk.scheme = auto, for a manually configured host: does this server want
 * https? One unauthenticated probe (SensESP #1057 -- never hand the token to
 * a host that has not been established as ours), redirects off, once per
 * server selection. *out_port receives the port to use, which a redirect may
 * change. Blocking, several seconds; SK task only.
 */
espos_sk_probe_t espos_sk_http_probe_https(const char *host, uint16_t port, uint16_t *out_port);

/* "<scheme>://host:port/path"; ESP_ERR_INVALID_SIZE when it does not fit. */
esp_err_t espos_sk_http_build_url(const espos_sk_server_t *srv, const char *scheme, const char *path, char *out, size_t n);
/* "/signalk/v1/api/vessels/self/<a/b/c><suffix>" from a dotted path. */
esp_err_t espos_sk_http_self_path(const char *sk_path, const char *suffix, char *out, size_t n);

/* Discovery (blocking ~3 s): discovery.c / discovery_sim.c */
esp_err_t espos_sk_discovery_init(const char *hostname);
size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max);

/* HTTP endpoints: api_sk.c */
esp_err_t espos_sk_register_api(void);

/* Forget the pinned certificate and retry now (DELETE /api/v1/sk/tls).
 * Queued to the SK task, which owns both the anchor and the machine. */
esp_err_t espos_sk_tls_reset_now(void);

/* WebSocket delta stream + meta reconciliation + health (sk_ws.c). */
/* The meta table the implementation allocates. CONFIG_ESPOS_SK_MAX_META
 * tunes it; ESPOS_SK_MAX_META (the public header) is what the API promises,
 * and the build fails below rather than silently accepting fewer
 * declarations than callers were told they could make. */
#ifdef CONFIG_ESPOS_SK_MAX_META
#define ESPOS_SK_META_CAP CONFIG_ESPOS_SK_MAX_META
#else
#define ESPOS_SK_META_CAP ESPOS_SK_MAX_META
#endif

esp_err_t espos_sk_ws_start(void);
void espos_sk_ws_stop(void);
void espos_sk_ws_config_changed(void);
/* Meta GET/PUT (sk_http.c). *out_meta receives the JSON object text (malloc'ed) or NULL if none. */
int espos_sk_http_get_meta(const espos_sk_server_t *srv, const char *token, const char *path, char **out_meta);
int espos_sk_http_put_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *meta_json);
/* JSON snippet for the status document. */
char *espos_sk_ws_status_json(void);
bool espos_sk_stream_allowed(void);

/* SignalK clock fallback: follows navigation.datetime while espos_time is
 * unset, and stands down once a better source syncs (sk_time.c). */
void espos_sk_time_start(void);
void espos_sk_time_stop(void);

#ifdef __cplusplus
}
#endif

/* sk_inbound.c ↔ sk_ws.c */
void espos_sk_inbound_set_connected(bool connected);
char *espos_sk_inbound_take_frame(void);            /* malloc'ed; NULL when nothing to send */
bool espos_sk_inbound_handle_frame(const char *json, size_t len, char *err_out, size_t err_size); /* false = server error frame */
void espos_sk_inbound_tick(uint32_t now_ms);
void espos_sk_inbound_stats(espos_sk_ws_status_t *st);
