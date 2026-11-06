/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h" /* before ESPOS_SK_META_CAP: every file must see the same table size */
#include "espos_sk.h"
#include "espos_sk_delta.h"
#include "espos_sk_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent device state (NVS namespace "skstate"): sk_store.c */
esp_err_t espos_sk_store_load(espos_sk_tok_store_t *out, char client_id[40]);
esp_err_t espos_sk_store_save(const espos_sk_tok_store_t *st);
/* The device's own vessel UUID, generated and stored on first use; "" and an
 * error when NVS cannot hold it. */
esp_err_t espos_sk_store_vessel_uuid(char uuid[40]);

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

/* What discovery must not lose: the anchored server, chosen the way the
 * merge and select_server() choose it (espos_sk_find_anchored). */
typedef struct {
    const espos_sk_anchor_t *anchor;
    const char *cur_host; /* the server in use, "" for none */
    uint16_t cur_port;
    bool self_fallback;
} espos_sk_keep_t;

/* Add one discovery answer to out[0..*n) of max. A server already listed is
 * replaced (the later answer wins, which makes https win over a stale http
 * record). On a full list the anchored server still gets in: it replaces the
 * listed entry find_anchored would pick when it would pick the newcomer over
 * that one, else the last slot. Answers come plaintext first, and a crowded
 * network must not crowd out the server the device is paired with. keep may
 * be NULL. */
void espos_sk_disc_add(espos_sk_discovered_t *out, size_t *n, size_t max, const espos_sk_discovered_t *d,
                       const espos_sk_keep_t *keep);

/* One discovery round merged into the server table. Pure: the caller holds
 * the lock and owns every buffer. */
typedef struct {
    const espos_sk_discovered_t *table; /* the list before this round */
    const uint32_t *table_avoid;        /* its avoid_until_ms stamps */
    size_t table_n;
    const espos_sk_discovered_t *found; /* this round's answers */
    size_t found_n;
    const espos_sk_anchor_t *anchor;
    const char *cur_host; /* the server in use, "" for none */
    uint16_t cur_port;
    bool self_fallback;
    uint32_t now_ms;
    uint32_t carry_ms; /* how long an unanswered entry is kept */
} espos_sk_merge_in_t;
/* Fills out[] and out_avoid[] (cap entries each; out_avoid is zeroed first)
 * and returns how many entries out[] holds. The anchored server takes the
 * first slot, then this round's answers, then carried-over entries. */
size_t espos_sk_merge_servers(const espos_sk_merge_in_t *in, espos_sk_discovered_t *out, uint32_t *out_avoid,
                              size_t cap);

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
/* keep (may be NULL) is passed to espos_sk_disc_add for every answer. */
size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max, const espos_sk_keep_t *keep);

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

/* The subscription, server and token tables the implementation allocates.
 * The public caps in espos_sk.h / espos_sk_token_sm.h stay literals for
 * callers and the ABI; these tune what the device actually reserves. */
#ifdef CONFIG_ESPOS_SK_MAX_SUBS
#define ESPOS_SK_SUBS_CAP CONFIG_ESPOS_SK_MAX_SUBS
#else
#define ESPOS_SK_SUBS_CAP ESPOS_SK_MAX_SUBS
#endif
#ifdef CONFIG_ESPOS_SK_MAX_SERVERS
#define ESPOS_SK_SERVERS_CAP CONFIG_ESPOS_SK_MAX_SERVERS
#else
#define ESPOS_SK_SERVERS_CAP ESPOS_SK_MAX_SERVERS
#endif
#ifdef CONFIG_ESPOS_SK_TOKEN_MAX
#define ESPOS_SK_TOKEN_CAP CONFIG_ESPOS_SK_TOKEN_MAX
#else
#define ESPOS_SK_TOKEN_CAP ESPOS_SK_TOKEN_MAX
#endif
/* The reason an approved token that does not fit is refused, wherever the
 * length is caught: the reply parser or the token machine. */
#define ESPOS_SK_TOKEN_TOO_LONG "token longer than CONFIG_ESPOS_SK_TOKEN_MAX allows; raise it, then retry"

esp_err_t espos_sk_ws_start(void);
void espos_sk_ws_stop(void);
void espos_sk_ws_config_changed(void);
/* What a meta GET found in a 200 reply. */
typedef enum {
    ESPOS_SK_META_NONE,       /* no meta object, or an empty one */
    ESPOS_SK_META_PRESENT,    /* the server has meta for the path */
    ESPOS_SK_META_UNREADABLE, /* a body cut off or not JSON: the server's, so never overwrite it */
    ESPOS_SK_META_OURS,       /* the text of ours a later declaration replaced, unedited */
} espos_sk_meta_seen_t;
/* Meta GET/PUT (sk_http.c); each returns the HTTP status, 0 when no answer came. */
/* ours, when not NULL, is a text the device sent and has since replaced:
 * meta that still holds it is reported as ESPOS_SK_META_OURS. */
int espos_sk_http_get_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *ours,
                           espos_sk_meta_seen_t *seen);
int espos_sk_http_put_meta(const espos_sk_server_t *srv, const char *token, const char *path, const char *meta_json);

/* One meta leg's outcome for the reconciliation round (sk_meta.c, pure). */
typedef enum {
    ESPOS_SK_META_DONE,         /* the server has meta for the path, its own or ours */
    ESPOS_SK_META_PUT,          /* it has none: send ours */
    ESPOS_SK_META_LATER,        /* a 4xx about this path: next path, this one on the retry */
    ESPOS_SK_META_UNAUTHORIZED, /* token refused: report it and end the round */
    ESPOS_SK_META_BACK_OFF,     /* no answer or a 5xx: end the round, the next leg would fare the same */
} espos_sk_meta_step_t;
espos_sk_meta_step_t espos_sk_meta_after_get(int status, espos_sk_meta_seen_t seen);
espos_sk_meta_step_t espos_sk_meta_after_put(int status);
/* True when the server's meta still carries every field of ours unchanged. */
bool espos_sk_meta_holds(const char *server_json, const char *ours_json);

/* The declared-meta table and its reconciliation bookkeeping (sk_meta.c,
 * pure: sk_ws.c holds one under its lock, the host tests drive it). */
typedef struct {
    char path[ESPOS_SK_PATH_MAX];
    char *meta_json;
    uint32_t period_ms;
    uint32_t gen; /* bumped by every declaration and connect, so a round knows its answer still holds */
    bool reconciled;
    char *stale; /* a text of ours the server took after a later declaration replaced it */
} espos_sk_meta_entry_t;

typedef struct {
    espos_sk_meta_entry_t e[ESPOS_SK_META_CAP];
    size_t n;
    bool dirty;           /* something left to reconcile */
    uint32_t round;       /* failed rounds in a row, for the retry backoff */
    uint32_t retry_at_ms; /* while round > 0, no round before this */
    size_t next;          /* where the next round starts: past the path the last one stopped at */
    uint32_t epoch;       /* bumped with any entry's gen, so a round knows its backoff still applies */
    uint32_t conn;        /* bumped by every connect, so a round knows its server is still the one */
} espos_sk_meta_table_t;

/* Takes ownership of meta_json. ESP_OK when recorded or unchanged,
 * ESP_ERR_NO_MEM when the table is full. */
esp_err_t espos_sk_meta_declare(espos_sk_meta_table_t *t, const char *path, char *meta_json, uint32_t period_ms);
/* A new connection: every path is asked again, and answers still in flight
 * for the old one are voided. */
void espos_sk_meta_connected(espos_sk_meta_table_t *t);
/* 0 when a round is due, else ms until one is; UINT32_MAX for nothing to do. */
uint32_t espos_sk_meta_wait(const espos_sk_meta_table_t *t, bool connected, uint32_t now_ms);
/* The index a round's k-th leg works on, k < n as taken at the round's start. */
size_t espos_sk_meta_index(const espos_sk_meta_table_t *t, size_t n, size_t k);
/* Records one path's outcome; gen is the entry's as the leg began. */
void espos_sk_meta_leg_done(espos_sk_meta_table_t *t, size_t i, uint32_t gen, espos_sk_meta_step_t step);
/* Records a PUT the server took of sent, which it takes ownership of; gen
 * and conn are the entry's and the table's as the leg began. */
void espos_sk_meta_sent(espos_sk_meta_table_t *t, size_t i, uint32_t gen, uint32_t conn, char *sent);
/* Ends a round: delay_ms is the backoff for t->round, used when paths are
 * left. Returns the delay set, 0 when every path is reconciled. */
uint32_t espos_sk_meta_round_end(espos_sk_meta_table_t *t, uint32_t now_ms, uint32_t delay_ms);

/* Meta reconciliation, driven by the client task (sk_ws.c holds the table).
 * The wait is UINT32_MAX while there is nothing to do: no stream, or every
 * declared path reconciled. The round makes blocking HTTP calls. */
uint32_t espos_sk_ws_meta_wait_ms(void);
void espos_sk_ws_meta_round(void);
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
