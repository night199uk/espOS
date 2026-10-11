/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sk core: one task owns discovery, server selection and the token
 * state machine. Everything else (HTTP handlers, config changes, other
 * components) talks to it through a command queue and reads a snapshot
 * under a mutex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_health.h"
#include "espos_httpd_sse.h"
#include "espos_net.h"
#include "espos_sk.h"
#include "espos_sk_priv.h"
#if CONFIG_ESPOS_SK_TLS
#include "espos_sk_tls.h"
#endif

static const char *TAG = "espos_sk";

typedef enum { CMD_CONFIG,
               CMD_DISCOVER,
               CMD_REQUEST,
               CMD_TOKEN,
               CMD_FORGET,
               CMD_UNAUTHORIZED,
               CMD_CERT_ERROR,
               CMD_TLS_RESET,
               CMD_STOP } cmd_type_t;
typedef struct {
    cmd_type_t type;
    char *str; /* CMD_TOKEN: malloc'ed token; CMD_CERT_ERROR: malloc'ed reason */
} cmd_t;

typedef enum { ACT_NONE,
               ACT_REQUEST,
               ACT_POLL,
               ACT_VERIFY } action_t;

static struct {
    TaskHandle_t task;
    QueueHandle_t cmds;
    SemaphoreHandle_t lock;          /* protects snapshot + servers */
    bool started;
    bool api_registered;

    /* task-private */
    espos_sk_tok_sm_t sm;
    espos_sk_tok_cfg_t cfg;
    action_t action;
    char action_href[ESPOS_SK_HREF_MAX];
    char action_token[ESPOS_SK_TOKEN_MAX];
    uint32_t timer_due_ms;           /* 0 = none */
    uint32_t discover_due_ms;
    bool discovery_enabled;
    uint32_t discover_interval_ms;
    char cfg_self[ESPOS_SK_SELF_MAX];
    bool cfg_pin;
    char cfg_host[ESPOS_SK_HOST_MAX];
    uint16_t cfg_port;
    /* sk.scheme: 0 = auto, 1 = http, 2 = https. `auto` is not a value the
     * transports can use, so it is resolved per server in select_server()
     * and the answer lives in the chosen espos_sk_server_t::tls. */
    uint8_t cfg_scheme;
    /* One probe per (host, port) selection, cached: the answer is a property
     * of the server, and asking again on every re-election would put an
     * unauthenticated round trip in front of every reconnect. */
    char probed_host[ESPOS_SK_HOST_MAX];
    uint16_t probed_port;
    bool probed_tls;
    uint16_t probed_use_port; /* a redirect may name a different port */
    bool probed_valid;
    /* The chosen server's scheme is a guess (plain), not a probe's answer:
     * there was no network at selection, or nothing answered. Asked again
     * when the network comes up and after every leg; see resolve_scheme(). */
    bool scheme_provisional;
    bool have_server;
    espos_sk_server_t server;
    char server_source[12];          /* "manual" | "discovered" | "pinned" | "" */
    espos_sk_tok_state_t narrated;   /* last token state announced on the log / event bus */

    /* shared snapshot */
    espos_sk_tok_status_t snap;
    char snap_token[ESPOS_SK_TOKEN_MAX];
    char client_id[40];
    espos_sk_discovered_t servers[ESPOS_SK_MAX_SERVERS];
    uint32_t avoid_until_ms[ESPOS_SK_MAX_SERVERS]; /* unreachable discovered servers are skipped for a while */
    /* Discovery rounds, among those anyone answered, in which the anchored
     * address (anchor_self_fallback) was not heard; keyed by that address. */
    char anchor_unheard_host[ESPOS_SK_HOST_MAX];
    uint16_t anchor_unheard_port;
    uint32_t anchor_unheard_rounds;
    size_t server_count;
    uint32_t last_discovery_ms;
    bool net_was_up;
    char snap_source[12];
    char hostname[33];
} s;

/* Set once by espos_start() before the first config load; a plain static
 * because it is read on the SK task and written before that task exists. */
static char s_app_name[33];

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* A deadline that is never 0 (0 means "none"; the tick count starts at 0). */
static uint32_t at(uint32_t ms)
{
    uint32_t t = now_ms() + ms;
    return t ? t : 1;
}

static void lock(void) { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

/* -------------------------------------------------------- SM port */

static void p_request(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg)
{
    (void)ctx;
    (void)srv;
    (void)cfg;
    s.action = ACT_REQUEST;
}
static void p_poll(void *ctx, const espos_sk_server_t *srv, const char *href)
{
    (void)ctx;
    (void)srv;
    s.action = ACT_POLL;
    snprintf(s.action_href, sizeof(s.action_href), "%s", href);
}
static void p_verify(void *ctx, const espos_sk_server_t *srv, const char *token)
{
    (void)ctx;
    (void)srv;
    s.action = ACT_VERIFY;
    snprintf(s.action_token, sizeof(s.action_token), "%s", token);
}
static void p_save(void *ctx, const espos_sk_tok_store_t *st)
{
    (void)ctx;
    espos_sk_store_save(st);
}
static void p_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    s.timer_due_ms = at(ms);
}
static void p_cancel(void *ctx)
{
    (void)ctx;
    s.timer_due_ms = 0;
}
static uint32_t p_now(void *ctx)
{
    (void)ctx;
    return now_ms();
}
static uint32_t p_random(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static char *status_json_from(const espos_sk_tok_status_t *st, const char *source);

/* The transitions a newcomer sits at the monitor waiting for, said in plain
 * words, plus the bus events for them. SK task only: the machine never
 * moves anywhere else. State changes, not every status change — APPROVED
 * re-verifies periodically and must not repeat itself. */
static void narrate_token_state(void)
{
    if (s.sm.st.state == s.narrated) {
        return;
    }
    s.narrated = s.sm.st.state;
    switch (s.sm.st.state) {
    case ESPOS_SK_TOK_REQUESTED:
        ESP_LOGI(TAG, "access requested — approve it in the server UI: Security → Access Requests");
        break;
    case ESPOS_SK_TOK_APPROVED:
        ESP_LOGI(TAG, "approved, streaming");
        (void)espos_event_post(ESPOS_EVENT_SK_TOKEN_APPROVED, NULL, 0);
        break;
    case ESPOS_SK_TOK_OPEN:
        ESP_LOGI(TAG, "server security is off: no token needed, streaming");
        break;
    case ESPOS_SK_TOK_DENIED:
        ESP_LOGW(TAG, "access denied by the server — request again from the device's web UI when it is allowed");
        break;
    case ESPOS_SK_TOK_CERT_ERROR:
        ESP_LOGW(TAG, "%s — trust it from the device's web UI (SignalK page) if the server's certificate "
                      "was legitimately replaced",
                 s.sm.st.last_error);
        break;
    default:
        break;
    }
    /* Not fatal: a device that cannot reach its server is still a device
     * doing its job locally, and rebooting would not fetch a new certificate.
     * WARN so it surfaces as a notification and on the health page, and
     * clears by itself the moment a connection succeeds. */
    (void)espos_health_report("skCertificate",
                              s.sm.st.state == ESPOS_SK_TOK_CERT_ERROR ? ESPOS_HEALTH_ALARM : ESPOS_HEALTH_NORMAL,
                              s.sm.st.state == ESPOS_SK_TOK_CERT_ERROR ? s.sm.st.last_error : "");
}

static void p_status_changed(void *ctx)
{
    (void)ctx;
    narrate_token_state();
    lock();
    s.snap = s.sm.st;
    snprintf(s.snap_token, sizeof(s.snap_token), "%s", espos_sk_tok_token(&s.sm));
    snprintf(s.snap_source, sizeof(s.snap_source), "%s", s.server_source);
    unlock();
    char *json = status_json_from(&s.sm.st, s.server_source);
    if (json) {
        espos_httpd_sse_publish("sk", json);
        free(json);
    }
}

static const espos_sk_tok_port_t k_port = {
    .http_request = p_request,
    .http_poll = p_poll,
    .http_verify = p_verify,
    .store_save = p_save,
    .arm_timer = p_arm,
    .cancel_timer = p_cancel,
    .now_ms = p_now,
    .random = p_random,
    .status_changed = p_status_changed,
};

/* ------------------------------------------------------ config load */

static void load_cfg(void)
{
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DISCOVERY, &s.discovery_enabled);
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_SELF, s.cfg_self, sizeof(s.cfg_self), NULL);
    espos_config_get_bool(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_PIN, &s.cfg_pin);
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_HOST, s.cfg_host, sizeof(s.cfg_host), NULL);
    int32_t v = 80;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SERVER_PORT, &v);
    s.cfg_port = (uint16_t)v;
    char scheme[8] = "auto";
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_SCHEME, scheme, sizeof(scheme), NULL);
    uint8_t sc = strcmp(scheme, "https") == 0 ? 2 : strcmp(scheme, "http") == 0 ? 1
                                                                                : 0;
#if !CONFIG_ESPOS_SK_TLS
    /* Say it once, loudly, rather than quietly talking plaintext to a server
     * the operator believes is protected. `auto` is not a complaint: without
     * the transports it simply resolves to http, which is the honest answer. */
    if (sc == 2 && s.cfg_scheme != 2) {
        ESP_LOGW(TAG, "sk.scheme is https but this firmware was built without "
                      "CONFIG_ESPOS_SK_TLS — continuing over http/ws");
    }
    sc = 1;
#endif
    if (sc != s.cfg_scheme) {
        s.probed_valid = false; /* the cached probe answered a different question */
    }
    s.cfg_scheme = sc;
#if CONFIG_ESPOS_SK_TLS
    {
        char trust[8] = "tofu";
        espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_TLS_TRUST, trust, sizeof(trust), NULL);
        /* The PEM is a blob and may be absent; a NUL-terminated copy is what
         * mbedtls_x509_crt_parse wants for PEM input. */
        static char pem[CONFIG_ESPOS_SK_TLS_CA_MAX + 1];
        size_t len = CONFIG_ESPOS_SK_TLS_CA_MAX;
        if (espos_config_get_blob(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_CA_PEM, pem, &len) != ESP_OK) {
            len = 0;
        }
        pem[len] = '\0';
        espos_sk_tls_set_trust(strcmp(trust, "bundle") == 0 ? ESPOS_SK_TLS_TRUST_BUNDLE
                               : strcmp(trust, "ca") == 0   ? ESPOS_SK_TLS_TRUST_CA
                                                            : ESPOS_SK_TLS_TRUST_TOFU,
                               pem);
    }
#endif
    v = 60;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DISCOVER_S, &v);
    s.discover_interval_ms = (uint32_t)v * 1000;

    espos_sk_tok_cfg_t c = { 0 };
    snprintf(c.client_id, sizeof(c.client_id), "%s", s.client_id);
    char d[65] = { 0 };
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_DESCRIPTION, d, sizeof(d), NULL);
    if (d[0]) {
        snprintf(c.description, sizeof(c.description), "%s", d);
    } else {
        snprintf(c.description, sizeof(c.description), "%s %s", s_app_name[0] ? s_app_name : "espOS", s.hostname);
    }
    espos_config_get_str(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_PERMISSIONS, c.permissions, sizeof(c.permissions), NULL);
    v = 60;
    espos_config_get_i32(ESPOS_CFG_NS_SK, ESPOS_CFG_SK_CHECK_S, &v);
    c.check_interval_ms = (uint32_t)v * 1000;
    lock();
    s.cfg = c; /* also read by status_json_from() on other tasks */
    unlock();
    espos_sk_tok_event(&s.sm, ESPOS_SK_EV_CONFIG, &c);
}

/* ---------------------------------------------------- server choice */

/* Periodic discovery is pointless once a manual host is configured: the
 * address is already known, select_server() reads the discovered entries
 * only for that address's self and scheme, and browsing on regardless only
 * spends radio time and mDNS traffic. Setting an explicit address should
 * mean exactly that — talk to this server, stop looking for others. The
 * list keeps what was last seen, for the web UI; POST /sk/discover still
 * refreshes it. */
static inline bool discovery_wanted(void)
{
    return s.discovery_enabled && !s.cfg_host[0];
}

/* http or https for this server? sk.scheme forces one when it is not "auto".
 *
 * "auto" asks the network. A discovered server already answered: signalk-server
 * publishes _signalk-https._tcp instead of _signalk-http._tcp when its `ssl`
 * setting is on, so `advertised` is the server's own statement and needs no
 * round trip. A manual host has said nothing, so it gets one probe — GET
 * http://host:port/signalk with redirects off, no token — and the answer is
 * cached against (host, port) so a reconnect does not repeat it.
 *
 * `advertised` is a tri-state: <0 = nothing advertised (manual host), 0 = the
 * plaintext service, 1 = the https one.
 *
 * Task-private; may block for seconds on the probe.
 */
static bool resolve_scheme(espos_sk_server_t *srv, int advertised)
{
#if !CONFIG_ESPOS_SK_TLS
    (void)advertised;
    (void)srv;
    return false;
#else
    if (s.cfg_scheme == 1) {
        return false;
    }
    if (s.cfg_scheme == 2) {
        return true;
    }
    if (advertised >= 0) {
        return advertised != 0;
    }
    if (s.probed_valid && strcmp(s.probed_host, srv->host) == 0 && s.probed_port == srv->port) {
        srv->port = s.probed_use_port;
        return s.probed_tls;
    }
    /* Plain for now, provisional, whenever there is no answer to go on: with
     * no network the probe cannot reach anything, and a host that does not
     * respond says nothing about its scheme. Both used to be cached as "plain
     * http" for (host, port), so a manual https server was never found by a
     * device that boots before its network is up -- every device -- or before
     * the server does, which on a boat is the usual order. The network coming
     * up and every leg that runs on the guess ask again. */
    if (!espos_net_is_up()) {
        s.scheme_provisional = true;
        return false;
    }
    uint16_t use = srv->port;
    espos_sk_probe_t probe = espos_sk_http_probe_https(srv->host, srv->port, &use);
    if (probe == ESPOS_SK_PROBE_NO_ANSWER) {
        s.scheme_provisional = true;
        return false;
    }
    bool tls = probe == ESPOS_SK_PROBE_TLS;
    snprintf(s.probed_host, sizeof(s.probed_host), "%s", srv->host);
    s.probed_port = srv->port;
    s.probed_use_port = use;
    s.probed_tls = tls;
    s.probed_valid = true;
    srv->port = use;
    return tls;
#endif
}

/* How long a discovered entry outlives its last mDNS answer. */
static uint32_t carry_window_ms(void)
{
    return 2 * s.discover_interval_ms + 5000;
}

/* Seen recently enough to be believed. With a manual host the list is no
 * longer refreshed, so its entries can be days old. Call under lock(). */
static bool entry_fresh(const espos_sk_discovered_t *d, uint32_t t)
{
    return (uint32_t)(t - d->seen_ms) < carry_window_ms();
}

/* Drop what entry_fresh() no longer believes, as a discovery round would. */
static void prune_stale_servers(void)
{
    uint32_t t = now_ms();
    size_t m = 0;
    lock();
    for (size_t i = 0; i < s.server_count; i++) {
        if (entry_fresh(&s.servers[i], t)) {
            s.avoid_until_ms[m] = s.avoid_until_ms[i];
            s.servers[m++] = s.servers[i];
        }
    }
    for (size_t i = m; i < s.server_count; i++) {
        s.avoid_until_ms[i] = 0;
    }
    s.server_count = m;
    unlock();
}

/* A server with the anchor's self URN but another address is a twin
 * (sk_select.c) until the anchored address has gone unheard for this many
 * answered rounds; after that it is taken to be our server at a new address.
 * Two, so that neither one lossy round nor the first round after boot hands
 * the pairing over. */
#define ANCHOR_UNHEARD_ROUNDS 2

static bool is_anchor_key(const espos_sk_anchor_t *a)
{
    return strcmp(s.anchor_unheard_host, a->host) == 0 && s.anchor_unheard_port == a->port;
}

/* Count this round against the anchored address if it was not among the
 * answers. A round nobody answered (the network is down or coming back) says
 * nothing either way and leaves the count alone: counting it would let an
 * outage hand the pairing to whichever twin answers first afterwards, and
 * resetting on it would let a flaky link postpone a real move for ever. Lock
 * held; run_discovery() only. */
static void anchor_note_answers(const espos_sk_anchor_t *a, const espos_sk_discovered_t *found, size_t n)
{
    if (!a->host[0] || n == 0) {
        return;
    }
    if (!is_anchor_key(a)) {
        snprintf(s.anchor_unheard_host, sizeof(s.anchor_unheard_host), "%s", a->host);
        s.anchor_unheard_port = a->port;
        s.anchor_unheard_rounds = 0;
    }
    for (size_t k = 0; k < n; k++) {
        if (espos_sk_is_anchor(&found[k], a)) {
            s.anchor_unheard_rounds = 0;
            return;
        }
    }
    s.anchor_unheard_rounds++;
}

/* May the anchor fall back to its self URN? Yes when it has no address (older
 * firmware stored none), or when that address has aged out of the list and
 * gone unheard for ANCHOR_UNHEARD_ROUNDS answered rounds. While it is still in
 * the list (carried over a lossy round) it is matched by address anyway. Lock
 * held. */
static bool anchor_self_fallback(const espos_sk_anchor_t *a)
{
    if (!a->host[0]) {
        return true;
    }
    for (size_t i = 0; i < s.server_count; i++) {
        if (espos_sk_is_anchor(&s.servers[i], a)) {
            return false;
        }
    }
    return is_anchor_key(a) && s.anchor_unheard_rounds >= ANCHOR_UNHEARD_ROUNDS;
}

static void publish_servers(void);

/* Pick the server per config: manual host wins; else the discovered server
 * with the preferred self; else the server our token or pending request
 * belongs to; else the "master", then any server, in election order
 * (espos_sk_disc_before). Task-private. Publishes the server list when the
 * choice moved, so its "selected" flags follow every caller; returns
 * whether it did. */
static bool select_server(void)
{
    espos_sk_server_t chosen = { 0 };
    const char *source = "";
    bool have = false;
    /* <0 = the server did not advertise a scheme (a manual host); 0/1 = it did. */
    int discovered_tls = -1;
    if (s.cfg_host[0]) {
        snprintf(chosen.host, sizeof(chosen.host), "%s", s.cfg_host);
        chosen.port = s.cfg_port;
        /* self may be known from a discovered entry with the same host:port */
        lock();
        uint32_t t = now_ms();
        for (size_t i = 0; i < s.server_count; i++) {
            if (entry_fresh(&s.servers[i], t) && strcmp(s.servers[i].host, chosen.host) == 0 &&
                s.servers[i].port == chosen.port) {
                snprintf(chosen.self, sizeof(chosen.self), "%s", s.servers[i].self);
                /* A manual host discovery happens to have seen has already
                 * told us its scheme; that beats a probe, which is only the
                 * fallback for an address nothing advertised. */
                discovered_tls = s.servers[i].tls ? 1 : 0;
                break;
            }
        }
        unlock();
        source = "manual";
        have = true;
    } else {
        lock();
        int idx = -1;
        uint32_t t = now_ms();
        const char *cur_host = s.have_server ? s.server.host : "";
        uint16_t cur_port = s.have_server ? s.server.port : 0;
        espos_sk_anchor_t anchor;
        espos_sk_store_anchor(&s.sm.store, &anchor);
        bool fallback = anchor_self_fallback(&anchor);
        if (s.cfg_self[0]) {
            /* The configured self, at the address our token is for when it
             * is that server's -- the same twin problem as below. */
            if (strcmp(anchor.self, s.cfg_self) != 0) {
                anchor.host = "";
                anchor.port = 0;
            }
            anchor.self = s.cfg_self;
            idx = espos_sk_find_anchored(s.servers, s.server_count, &anchor, cur_host, cur_port, fallback);
        } else {
            /* Sticky and deterministic: (1) the server our token / pending
             * request belongs to, by address and then by self URN
             * (sk_select.c says why self alone is not enough), (2) reachable
             * "master" servers by self URN, (3) any reachable server by self
             * URN. mDNS answer order is not stable, so never "first in the
             * list". */
            idx = espos_sk_find_anchored(s.servers, s.server_count, &anchor, cur_host, cur_port, fallback);
            /* While the anchored address is only briefly silent, a server
             * with its self is a twin until proven otherwise: electing it
             * would cost the pairing just the same. */
            bool twins_held = idx < 0 && anchor.host[0] && !fallback;
            for (int pass = 0; pass < 2 && idx < 0; pass++) {
                for (size_t i = 0; i < s.server_count; i++) {
                    bool avoided = s.avoid_until_ms[i] && (int32_t)(s.avoid_until_ms[i] - t) > 0;
                    bool master = strstr(s.servers[i].roles, "master") != NULL;
                    if (avoided || (pass == 0 && !master) ||
                        (twins_held && strcmp(s.servers[i].self, anchor.self) == 0)) {
                        continue;
                    }
                    if (idx < 0 || espos_sk_disc_before(&s.servers[i], &s.servers[idx], cur_host, cur_port)) {
                        idx = (int)i;
                    }
                }
            }
        }
        /* Pinned: keep the server already in use even when this round of
         * discovery did not see it. Re-election is what moves a helm
         * display onto a different vessel's server mid-passage; some
         * installations would rather wait for their own to answer again. */
        if (idx < 0 && s.cfg_pin && s.have_server && s.server.host[0]) {
            chosen = s.server;
            source = "pinned";
            have = true;
            discovered_tls = s.server.tls ? 1 : 0; /* keep what it was elected with */
        }
        if (idx >= 0) {
            snprintf(chosen.host, sizeof(chosen.host), "%s", s.servers[idx].host);
            chosen.port = s.servers[idx].port;
            snprintf(chosen.self, sizeof(chosen.self), "%s", s.servers[idx].self);
            /* A stand-in for the anchored address, found by its self URN. */
            chosen.by_self = anchor.host[0] && anchor.self[0] && !espos_sk_is_anchor(&s.servers[idx], &anchor) &&
                             strcmp(s.servers[idx].self, anchor.self) == 0;
            discovered_tls = s.servers[idx].tls ? 1 : 0;
            source = "discovered";
            have = true;
        }
        unlock();
    }
    s.scheme_provisional = false; /* resolve_scheme() says so when it had to guess */
    chosen.tls = have && resolve_scheme(&chosen, discovered_tls);
    bool changed = have != s.have_server || (have && (strcmp(chosen.host, s.server.host) != 0 ||
                                                      chosen.port != s.server.port || chosen.tls != s.server.tls ||
                                                      strcmp(chosen.self, s.server.self) != 0));
    bool source_changed = strcmp(source, s.server_source) != 0;
    bool by_self_changed = have && !changed && chosen.by_self != s.server.by_self;
    lock(); /* espos_sk_servers_json() reads the choice on other tasks */
    s.have_server = have;
    s.server = chosen;
    unlock();
    snprintf(s.server_source, sizeof(s.server_source), "%s", source);
    if (!changed && source_changed) {
        p_status_changed(NULL); /* same server, different provenance: refresh the snapshot */
    }
    if (by_self_changed) {
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_SERVER, &chosen);
    }
    if (changed) {
        if (have) {
            /* The advertised instance name is what the operator knows the
             * server as; a manual host has none unless discovery saw it. */
            char name[48] = "";
            lock();
            for (size_t i = 0; i < s.server_count; i++) {
                if (strcmp(s.servers[i].host, chosen.host) == 0 && s.servers[i].port == chosen.port) {
                    snprintf(name, sizeof(name), "%s", s.servers[i].name);
                    break;
                }
            }
            unlock();
            ESP_LOGI(TAG, "found signalk-server \"%s\" at %s:%u (%s%s%s%s)", name[0] ? name : chosen.host,
                     chosen.host, chosen.port, source, chosen.tls ? ", tls" : "",
                     chosen.self[0] ? " " : "", chosen.self);
            espos_event_sk_server_t ev = { .port = chosen.port };
            snprintf(ev.host, sizeof(ev.host), "%s", chosen.host);
            (void)espos_event_post(ESPOS_EVENT_SK_SERVER_SELECTED, &ev, sizeof(ev));
        } else {
            ESP_LOGI(TAG, "no server (waiting for discovery or manual host)");
        }
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_SERVER, have ? &chosen : NULL);
    }
    /* After the choice is stored: a list published before it showed the
     * previous server as selected, and a pick in the list looked as if it
     * had not taken. */
    bool publish = changed || source_changed || by_self_changed;
    if (publish) {
        publish_servers();
    }
    return publish;
}

/* The server list as the web UI shows it. */
static void publish_servers(void)
{
    char *json = NULL;
    if (espos_sk_servers_json(&json) == ESP_OK) {
        espos_httpd_sse_publish("sk_servers", json);
        free(json);
    }
}

static void run_discovery(void)
{
    /* heap, not stack: two arrays of ~2 KiB each */
    espos_sk_discovered_t *found = calloc(ESPOS_SK_MAX_SERVERS, sizeof(*found));
    espos_sk_discovered_t *merged = calloc(ESPOS_SK_MAX_SERVERS, sizeof(*merged));
    if (!found || !merged) {
        free(found);
        free(merged);
        return;
    }
    size_t n = espos_sk_discovery_run(found, ESPOS_SK_MAX_SERVERS);
    uint32_t t = now_ms();
    lock();
    /* Merge: fresh results replace, but keep recently seen entries that
     * dropped out of one query (mDNS is lossy) for two intervals. */
    uint32_t avoid[ESPOS_SK_MAX_SERVERS] = { 0 };
    size_t m = 0;
    /* The server our token belongs to gets the first slot.
     *
     * Fresh results used to fill the array first, so on a network
     * advertising ESPOS_SK_MAX_SERVERS or more the carry-over loop below
     * — the whole defence against mDNS being lossy — could not run at
     * all. One missed answer then evicted the anchored server, the
     * anchor lookup in select_server() found nothing, and a perfectly
     * reachable server was dropped with "no server". Reserving its slot
     * first means a full list can never cost us the one entry that
     * matters.
     *
     * Reserved by address, not by self URN: a second server advertising
     * the same self (sk_select.c) would otherwise take the slot whenever
     * mDNS answered for it first. And only while it is as fresh as any
     * carried-over entry: a server that moved must age out at its old
     * address, or the stale entry would keep winning select_server()'s
     * address match and the device would never follow it. */
    espos_sk_anchor_t anchor;
    espos_sk_store_anchor(&s.sm.store, &anchor);
    anchor_note_answers(&anchor, found, n);
    int keep = espos_sk_find_anchored(s.servers, s.server_count, &anchor, s.have_server ? s.server.host : "",
                                      s.have_server ? s.server.port : 0, anchor_self_fallback(&anchor));
    bool keep_reserved = false;
    char keep_host[ESPOS_SK_HOST_MAX] = "";
    uint16_t keep_port = 0;
    if (keep >= 0) {
        /* Prefer this round's fresher copy if mDNS did answer for it. */
        bool fresh = false;
        for (size_t k = 0; k < n; k++) {
            if (espos_sk_same_addr(&found[k], s.servers[keep].host, s.servers[keep].port)) {
                merged[m] = found[k];
                merged[m].seen_ms = t;
                fresh = true;
                break;
            }
        }
        if (!fresh && (int32_t)(t - s.servers[keep].seen_ms) < (int32_t)carry_window_ms()) {
            merged[m] = s.servers[keep];
            fresh = true;
        }
        if (fresh) {
            snprintf(keep_host, sizeof(keep_host), "%s", merged[m].host);
            keep_port = merged[m].port;
            avoid[m] = s.avoid_until_ms[keep];
            m++;
            keep_reserved = true;
        }
    }
    for (size_t i = 0; i < n && m < ESPOS_SK_MAX_SERVERS; i++) {
        /* Skip only what the reservation above actually took. Testing
         * m > 0 instead would drop the anchored server whenever the
         * reservation did NOT run — the empty-s.servers[] case on a
         * fresh boot with a token restored from NVS, which is exactly
         * when it must not be lost. */
        if (keep_reserved && espos_sk_same_addr(&found[i], keep_host, keep_port)) {
            continue;
        }
        merged[m] = found[i];
        merged[m].seen_ms = t;
        for (size_t k = 0; k < s.server_count; k++) { /* carry the avoid stamp over */
            if (strcmp(s.servers[k].host, found[i].host) == 0 && s.servers[k].port == found[i].port) {
                avoid[m] = s.avoid_until_ms[k];
            }
        }
        m++;
    }
    for (size_t i = 0; i < s.server_count && m < ESPOS_SK_MAX_SERVERS; i++) {
        bool dup = false;
        for (size_t k = 0; k < m; k++) {   /* against what we kept, including the reserved anchor */
            if (strcmp(merged[k].host, s.servers[i].host) == 0 && merged[k].port == s.servers[i].port) {
                dup = true;
                break;
            }
        }
        if (!dup && (int32_t)(t - s.servers[i].seen_ms) < (int32_t)carry_window_ms()) {
            avoid[m] = s.avoid_until_ms[i];
            merged[m++] = s.servers[i];
        }
    }
    memcpy(s.servers, merged, m * sizeof(merged[0]));
    memcpy(s.avoid_until_ms, avoid, sizeof(avoid));
    s.server_count = m;
    s.last_discovery_ms = t;
    unlock();
    free(found);
    free(merged);
    ESP_LOGI(TAG, "discovery: %u server(s)", (unsigned)m);
    if (!select_server()) {
        publish_servers(); /* the entries changed even when the choice did not */
    }
}

/* ------------------------------------------------------------ task */

static void handle_cmd(const cmd_t *c)
{
    switch (c->type) {
    case CMD_CONFIG: {
        load_cfg();
        /* Only switching discovery off empties the list. A manual host keeps
         * it: the entries say which of them is the pinned one, and an empty
         * list after picking a row read as if the pick had been lost. Back
         * to discovery, the election must not run on what the manual host
         * left unrefreshed. */
        size_t before = s.server_count;
        if (!s.discovery_enabled) {
            lock();
            s.server_count = 0; /* discovered entries are no longer valid choices */
            unlock();
        } else if (!s.cfg_host[0]) {
            prune_stale_servers();
        }
        if (!select_server() && s.server_count != before) {
            publish_servers();
        }
        /* Browse now whenever discovery has just become wanted again —
         * discovery switched on, or a manual host cleared. Testing only
         * `discover_due_ms == 0` was not enough: setting a manual host
         * leaves the old due-time in place, so clearing it later left
         * discovery wanted but never rescheduled, and the device sat with
         * no server until something else happened to re-arm the timer. */
        if (discovery_wanted()) {
            s.discover_due_ms = at(0);
        } else {
            s.discover_due_ms = 0;
        }
        break;
    }
    case CMD_DISCOVER:
        /* Deliberately NOT discovery_wanted(): this is an explicit request
         * from POST /sk/discover, and the endpoint has already answered
         * "discovering". A manual host suppresses the periodic browse, but
         * silently doing nothing here would make the web UI's Discover
         * button lie — it is also the one way to see what else is on the
         * network while pinned to an address. Only the config switch turns
         * it off entirely. */
        if (s.discovery_enabled) {
            run_discovery();
        }
        s.discover_due_ms = at(s.discover_interval_ms);
        break;
    case CMD_REQUEST:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_RETRY, NULL);
        break;
    case CMD_TOKEN:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_MANUAL_TOKEN, c->str);
        break;
    case CMD_FORGET: {
        /* Drop the token. A pending request stays: the server holds it
         * anyway and would refuse a duplicate; polling just resumes. */
        s.sm.store.token[0] = '\0';
        s.sm.store.token_self[0] = '\0';
        s.sm.store.token_host[0] = '\0';
        s.sm.store.token_port = 0;
        espos_sk_store_save(&s.sm.store);
        s.sm.st.has_token = false;
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_RETRY, NULL);
        p_status_changed(NULL); /* even without a server the snapshot must show the loss */
        break;
    }
    case CMD_UNAUTHORIZED:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_UNAUTHORIZED, NULL);
        break;
    case CMD_CERT_ERROR:
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_CERT_ERROR, c->str);
        break;
    case CMD_TLS_RESET:
#if CONFIG_ESPOS_SK_TLS
        espos_sk_tls_reset();
        /* Nothing to poll: the only way to find out whether the new
         * certificate is acceptable is to try. Retry now rather than waiting
         * out the CERT_ERROR minute -- the operator just pressed the button
         * and is watching. */
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_RETRY, NULL);
#endif
        break;
    case CMD_STOP:
        break;
    }
}

/* A discovered server we cannot reach (wrong subnet, gone) must not hold
 * the machine hostage when others are available: sidestep it for a while. */
static void maybe_rotate_unreachable(void)
{
    /* cfg_self and cfg_pin both mean "this server or none": rotating away
     * would defeat the setting the moment the server had a bad minute. */
    if (s.sm.st.state != ESPOS_SK_TOK_ERROR || s.sm.st.last_http_status != 0 ||
        strcmp(s.server_source, "discovered") != 0 || s.cfg_self[0] || s.cfg_pin) {
        return;
    }
    lock();
    uint32_t t = now_ms();
    size_t alternatives = 0;
    for (size_t i = 0; i < s.server_count; i++) {
        bool is_current = strcmp(s.servers[i].host, s.server.host) == 0 && s.servers[i].port == s.server.port;
        bool avoided = s.avoid_until_ms[i] && (int32_t)(s.avoid_until_ms[i] - t) > 0;
        if (!is_current && !avoided) {
            alternatives++;
        }
    }
    if (alternatives == 0) {
        unlock();
        return; /* nothing better to try; the machine's own backoff handles the outage */
    }
    for (size_t i = 0; i < s.server_count; i++) {
        if (strcmp(s.servers[i].host, s.server.host) == 0 && s.servers[i].port == s.server.port) {
            s.avoid_until_ms[i] = at(5 * 60 * 1000);
            ESP_LOGW(TAG, "%s:%u unreachable; trying another server for 5 min", s.server.host, s.server.port);
        }
    }
    unlock();
    select_server();
}

static void run_action(void)
{
    action_t a = s.action;
    s.action = ACT_NONE;
    espos_sk_http_result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return;
    }
    espos_sk_server_t srv = s.server;
    switch (a) {
    case ACT_REQUEST:
        espos_sk_http_request(&srv, &s.cfg, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_REQUEST_RESULT, r);
        break;
    case ACT_POLL:
        espos_sk_http_poll(&srv, s.action_href, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_POLL_RESULT, r);
        break;
    case ACT_VERIFY:
        espos_sk_http_verify(&srv, s.action_token, r);
        espos_sk_tok_event(&s.sm, ESPOS_SK_EV_VERIFY_RESULT, r);
        break;
    default:
        break;
    }
    free(r);
    /* The scheme is a guess because the probe went unanswered: every leg that
     * runs on it is the cadence to ask again at, so a server that came up
     * after the device is reached over the scheme it speaks. Cheap when it
     * answers; paced by the machine's own backoff when it does not. */
    if (s.scheme_provisional && espos_net_is_up()) {
        select_server();
    }
    maybe_rotate_unreachable();
}

static void sk_task(void *arg)
{
    (void)arg;
    espos_sk_tok_store_t *store = calloc(1, sizeof(*store));
    char cid[40];
    espos_sk_store_load(store, cid);
    lock();
    strcpy(s.client_id, cid);
    unlock();
    load_cfg();
    espos_sk_tok_init(&s.sm, &k_port, NULL, &s.cfg, store);
    free(store);
    espos_sk_tok_event(&s.sm, ESPOS_SK_EV_START, NULL);
    s.discover_due_ms = at(2000); /* first pass shortly; re-triggered when the network comes up */
    /* Only with a network. Without one a manual host is handed to the machine
     * unreachable, and its first failed leg costs the full error backoff after
     * the network does come up -- ten seconds of a device on the cable doing
     * nothing. Discovered servers need no such care: there are none until
     * then. The up edge below selects. */
    s.net_was_up = espos_net_is_up();
    if (s.net_was_up) {
        select_server();
    }
    p_status_changed(NULL);

    for (;;) {
        /* 0. network came up: discover right away (mDNS is useless before),
         * and select a manual host that could not be selected, or probed,
         * without it */
        {
            bool up = espos_net_is_up();
            if (up && !s.net_was_up) {
                if (discovery_wanted()) {
                    s.discover_due_ms = at(0);
                }
                if (s.cfg_host[0] && (!s.have_server || s.scheme_provisional)) {
                    select_server();
                }
            }
            s.net_was_up = up;
        }
        /* 1. run whatever the machine asked for (blocking HTTP) */
        while (s.action != ACT_NONE) {
            run_action();
        }
        /* 2. timers */
        uint32_t t = now_ms();
        if (s.timer_due_ms && (int32_t)(t - s.timer_due_ms) >= 0) {
            s.timer_due_ms = 0;
            espos_sk_tok_event(&s.sm, ESPOS_SK_EV_TIMER, NULL);
            continue;
        }
        if (discovery_wanted() && s.discover_due_ms && (int32_t)(t - s.discover_due_ms) >= 0) {
            run_discovery();
            s.discover_due_ms = at(s.discover_interval_ms);
            continue;
        }
        /* 3. wait for a command or the next deadline */
        uint32_t wait = 1000;
        if (s.timer_due_ms) {
            int32_t d = (int32_t)(s.timer_due_ms - t);
            if (d < (int32_t)wait) wait = d > 0 ? (uint32_t)d : 0;
        }
        if (discovery_wanted() && s.discover_due_ms) {
            int32_t d = (int32_t)(s.discover_due_ms - t);
            if (d < (int32_t)wait) wait = d > 0 ? (uint32_t)d : 0;
        }
        cmd_t c;
        if (xQueueReceive(s.cmds, &c, pdMS_TO_TICKS(wait)) == pdTRUE) {
            if (c.type == CMD_STOP) {
                espos_sk_tok_event(&s.sm, ESPOS_SK_EV_STOP, NULL);
                break;
            }
            handle_cmd(&c);
            free(c.str);
        }
    }
    s.task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------- JSON */

static char *status_json_from(const espos_sk_tok_status_t *st, const char *source)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    uint32_t t = now_ms();
    cJSON *tok = cJSON_AddObjectToObject(root, "token");
    cJSON_AddStringToObject(tok, "state", espos_sk_tok_state_str(st->state));
    cJSON_AddBoolToObject(tok, "has_token", st->has_token);
    cJSON_AddBoolToObject(tok, "busy", st->busy);
    if (st->pending_href[0]) {
        cJSON_AddStringToObject(tok, "pending_href", st->pending_href);
        cJSON_AddNumberToObject(tok, "pending_s", (t - st->requested_since_ms) / 1000);
    }
    if (st->state == ESPOS_SK_TOK_APPROVED) {
        cJSON_AddNumberToObject(tok, "approved_s", (t - st->approved_since_ms) / 1000);
    }
    if (st->next_action_ms) {
        int32_t d = (int32_t)(st->next_action_ms - t);
        cJSON_AddNumberToObject(tok, "next_action_s", d > 0 ? d / 1000 : 0);
    }
    if (st->last_check_ms) {
        cJSON_AddNumberToObject(tok, "last_check_s", (t - st->last_check_ms) / 1000);
    }
    cJSON_AddNumberToObject(tok, "last_http_status", st->last_http_status);
    cJSON_AddStringToObject(tok, "last_error", st->last_error);
    cJSON *cnt = cJSON_AddObjectToObject(tok, "counts");
    cJSON_AddNumberToObject(cnt, "requests", st->request_count);
    cJSON_AddNumberToObject(cnt, "approved", st->approve_count);
    cJSON_AddNumberToObject(cnt, "denied", st->deny_count);
    cJSON_AddNumberToObject(cnt, "unauthorized", st->unauthorized_count);
    cJSON_AddNumberToObject(cnt, "cert_errors", st->cert_error_count);
    cJSON *srv = cJSON_AddObjectToObject(root, "server");
    if (st->has_server) {
        cJSON_AddStringToObject(srv, "host", st->server.host);
        cJSON_AddNumberToObject(srv, "port", st->server.port);
        cJSON_AddStringToObject(srv, "self", st->server.self);
        cJSON_AddStringToObject(srv, "source", source);
        /* The scheme actually in use, which under sk.scheme = auto is not
         * something the configuration can be read off. */
        cJSON_AddStringToObject(srv, "scheme", st->server.tls ? "https" : "http");
        cJSON_AddBoolToObject(srv, "by_self", st->server.by_self);
        lock();
        for (size_t i = 0; i < s.server_count; i++) {
            if (strcmp(s.servers[i].host, st->server.host) == 0 && s.servers[i].port == st->server.port) {
                cJSON_AddStringToObject(srv, "name", s.servers[i].name);
                cJSON_AddStringToObject(srv, "swname", s.servers[i].swname);
                cJSON_AddStringToObject(srv, "swvers", s.servers[i].swvers);
                break;
            }
        }
        unlock();
    } else {
        cJSON_AddStringToObject(srv, "source", "none");
    }
    char *wsj = espos_sk_ws_status_json();
    if (wsj) {
        cJSON *w = cJSON_Parse(wsj);
        free(wsj);
        if (w) {
            cJSON_AddItemToObject(root, "ws", w);
        }
    }
    cJSON *disc = cJSON_AddObjectToObject(root, "discovery");
    lock();
    cJSON_AddStringToObject(root, "client_id", s.client_id);
    cJSON_AddStringToObject(root, "description", s.cfg.description);
    cJSON_AddStringToObject(root, "permissions", s.cfg.permissions);
    /* What is actually running, not merely what the flag says: a manual
     * host suppresses discovery, and the UI should not claim otherwise. */
    cJSON_AddBoolToObject(disc, "enabled", discovery_wanted());
    cJSON_AddNumberToObject(disc, "count", (double)s.server_count);
    if (s.last_discovery_ms) {
        cJSON_AddNumberToObject(disc, "last_s", (t - s.last_discovery_ms) / 1000);
    } else {
        cJSON_AddNullToObject(disc, "last_s");
    }
    unlock();
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

esp_err_t espos_sk_status_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    espos_sk_tok_status_t st = s.snap;
    char source[12];
    strcpy(source, s.snap_source);
    unlock();
    *out_json = status_json_from(&st, source);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t espos_sk_servers_json(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "servers");
    uint32_t t = now_ms();
    lock();
    for (size_t i = 0; arr && i < s.server_count; i++) {
        const espos_sk_discovered_t *d = &s.servers[i];
        cJSON *e = cJSON_CreateObject();
        if (!e) {
            break;
        }
        cJSON_AddStringToObject(e, "host", d->host);
        cJSON_AddNumberToObject(e, "port", d->port);
        cJSON_AddStringToObject(e, "self", d->self);
        cJSON_AddStringToObject(e, "name", d->name);
        cJSON_AddStringToObject(e, "roles", d->roles);
        cJSON_AddStringToObject(e, "swname", d->swname);
        cJSON_AddStringToObject(e, "swvers", d->swvers);
        cJSON_AddStringToObject(e, "scheme", d->tls ? "https" : "http");
        cJSON_AddNumberToObject(e, "seen_s", (t - d->seen_ms) / 1000);
        cJSON_AddBoolToObject(e, "selected", s.have_server && strcmp(s.server.host, d->host) == 0 && s.server.port == d->port);
        cJSON_AddItemToArray(arr, e);
    }
    if (s.last_discovery_ms) {
        cJSON_AddNumberToObject(root, "last_s", (t - s.last_discovery_ms) / 1000);
    } else {
        cJSON_AddNullToObject(root, "last_s");
    }
    unlock();
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

/* -------------------------------------------------------- commands */

static esp_err_t post_cmd(cmd_type_t type, char *str)
{
    if (!s.cmds) {
        free(str);
        return ESP_ERR_INVALID_STATE;
    }
    cmd_t c = { .type = type, .str = str };
    if (xQueueSend(s.cmds, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(str);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t espos_sk_discover_now(void) { return post_cmd(CMD_DISCOVER, NULL); }
esp_err_t espos_sk_request_now(void) { return post_cmd(CMD_REQUEST, NULL); }
esp_err_t espos_sk_forget_token(void) { return post_cmd(CMD_FORGET, NULL); }
void espos_sk_report_unauthorized(void) { (void)post_cmd(CMD_UNAUTHORIZED, NULL); }

void espos_sk_report_cert_error(const char *reason)
{
    (void)post_cmd(CMD_CERT_ERROR, reason && reason[0] ? strdup(reason) : NULL);
}

esp_err_t espos_sk_tls_reset_now(void) { return post_cmd(CMD_TLS_RESET, NULL); }

esp_err_t espos_sk_set_token(const char *token)
{
    if (!token || !token[0] || strlen(token) >= ESPOS_SK_TOKEN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(token);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    return post_cmd(CMD_TOKEN, copy);
}

esp_err_t espos_sk_get_token(char *buf, size_t size)
{
    if (!buf || !size || !s.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    snprintf(buf, size, "%s", s.snap_token);
    unlock();
    return ESP_OK;
}

esp_err_t espos_sk_get_server(espos_sk_server_t *out)
{
    if (!out || !s.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    bool have = s.snap.has_server;
    if (have) {
        *out = s.snap.server;
    }
    unlock();
    return have ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t espos_sk_set_app_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_app_name, sizeof(s_app_name), "%s", name);
    return ESP_OK;
}

const char *espos_sk_client_id(void)
{
    return s.client_id;
}

/* The stream may run when the token is approved, or when the server has
 * security disabled (OPEN: no token needed). */
bool espos_sk_stream_allowed(void)
{
    if (!s.lock) {
        return false;
    }
    lock();
    bool ok = s.snap.state == ESPOS_SK_TOK_APPROVED || s.snap.state == ESPOS_SK_TOK_OPEN;
    unlock();
    return ok;
}

/* -------------------------------------------------------- lifecycle */

static void on_config_change(const char *ns, const char *key, void *arg)
{
    (void)key;
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_SK) == 0) {
        post_cmd(CMD_CONFIG, NULL);
        espos_sk_ws_config_changed();
    }
    /* net.hostname (the source label) is restart_required and espos_net
     * applies it at boot, so there is nothing to follow live here. */
}

esp_err_t espos_sk_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    /* Discovery is mDNS and the stream opens when the network is up; both
     * ask espos_net, which does not answer before espos_net_start() (and
     * that in turn needs the HTTP server, so the whole chain holds). Which
     * transport carries the link is not this component's business. */
    espos_net_status_t net;
    if (espos_net_get_status(&net) != ESP_OK) {
        ESP_LOGE(TAG, "espos_sk_start: call espos_net_start() first (or espos_start())");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        s.cmds = xQueueCreate(8, sizeof(cmd_t));
        if (!s.lock || !s.cmds) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* hostname for the request description: espos_net's, the same name the
     * mDNS responder answers to (net.hostname, default espos-<id>) */
    snprintf(s.hostname, sizeof(s.hostname), "%s", net.hostname[0] ? net.hostname : "espos");
    espos_sk_discovery_init(s.hostname);
    if (!s.api_registered) {
        ESP_ERROR_CHECK(espos_sk_register_api());
        s.api_registered = true;
    }
    espos_config_subscribe(on_config_change, NULL);
    s.started = true;
    if (xTaskCreate(sk_task, "espos_sk", CONFIG_ESPOS_SK_TASK_STACK, NULL, tskIDLE_PRIORITY + 3, &s.task) != pdPASS) {
        s.started = false;
        return ESP_ERR_NO_MEM;
    }
    espos_sk_time_start();
    return espos_sk_ws_start();
}

esp_err_t espos_sk_stop(void)
{
    if (!s.started) {
        return ESP_OK;
    }
    espos_config_unsubscribe(on_config_change, NULL);
    espos_sk_time_stop();
    espos_sk_ws_stop();
    post_cmd(CMD_STOP, NULL);
    for (int i = 0; i < 100 && s.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s.started = false;
    return ESP_OK;
}
