/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_net — the one place espOS answers "is the network up, and how".
 *
 * Every transport (espos_wifi today; Ethernet and 802.15.4 next) reports its
 * link into this component with espos_net_report(). espos_net keeps one
 * record per interface, picks the interface that carries the default route
 * (Ethernet over WiFi station over Thread when several are up), owns the
 * device's hostname (config key net.hostname, default espos-<id>) and its
 * mDNS responder (espos_mdns.h), and tells everyone else about changes:
 * ESPOS_EVENT_NETWORK_UP / NETWORK_DOWN on the event bus, espos_net_subscribe()
 * callbacks, the "net" SSE event and GET /api/v1/net/status.
 *
 * SignalK, OTA and mDNS depend on this component and not on a radio, which is
 * what lets a firmware leave espos_wifi out — an Ethernet gateway, an H-series
 * device without WiFi — without touching them.
 *
 * Threading: espos_net_get_status(), espos_net_is_up() and
 * espos_net_short_id() never call a driver or wait on one; a status snapshot
 * must not depend on a radio answering (the hosted ESP32-P4 rule, docs/wifi.md).
 * espos_net_report() is thread-safe and is called by a transport from its own
 * task. Subscribe callbacks run on that reporting task — for WiFi the espos_wifi
 * task — with no espos_net lock held: copy the status out and
 * return, never block there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer sizes of espos_net_status_t, NUL included. Values are part of the ABI. */
#define ESPOS_NET_IP_MAX       16 /* dotted IPv4 */
#define ESPOS_NET_IP6_MAX      40 /* textual IPv6 */
#define ESPOS_NET_HOSTNAME_MAX 33 /* 32 characters, the DHCP/mDNS label limit */
#define ESPOS_NET_SHORT_ID_MAX 5  /* "1a2b" */

/* The interfaces a transport can report. Values are ABI: append before _MAX,
 * never renumber. espos_net_sm_t is sized by ESPOS_NET_IF_MAX, so appending
 * one is an ESPOS_ABI_VERSION bump. */
typedef enum {
    ESPOS_NET_IF_NONE = 0,     /* no default route */
    ESPOS_NET_IF_WIFI_STA = 1, /* espos_wifi's station */
    ESPOS_NET_IF_ETH = 2,      /* wired Ethernet (espos_eth) */
    ESPOS_NET_IF_THREAD = 3,   /* 802.15.4 / Thread (future transport) */
    ESPOS_NET_IF_MAX = 4,
} espos_net_if_t;

/* One snapshot of the network as the rest of espOS sees it: the interface that
 * carries the default route and the device's identity on it. */
typedef struct {
    bool up;                             /* a default route exists */
    espos_net_if_t iface;                /* the interface carrying it; NONE when down */
    char ip[ESPOS_NET_IP_MAX];           /* "" when down */
    char netmask[ESPOS_NET_IP_MAX];      /* "" when down */
    char gateway[ESPOS_NET_IP_MAX];      /* "" when down or none */
    char ip6_ll[ESPOS_NET_IP6_MAX];      /* link-local IPv6 of that interface; "" when none or unknown */
    uint8_t mac[6];                      /* the base MAC (eFuse); all zero on the host */
    char hostname[ESPOS_NET_HOSTNAME_MAX]; /* what the device answers to as <hostname>.local */
    int8_t rssi;                         /* dBm of the WiFi station carrying the route; 0 on any other interface */
    uint32_t up_count;                   /* how often the default route came up (or moved) since boot */
    uint32_t up_since_ms;                /* milliseconds the current default route has been up; 0 when down */
} espos_net_status_t;

/**
 * Bring the network seam up: base MAC and short id, hostname (net.hostname,
 * moved once from a 0.7 wifi.hostname), the /net endpoints, the "net" SSE
 * event and the mDNS responder. Requires espos_init() (config) and
 * espos_httpd_start(); ESP_ERR_INVALID_STATE with one log line otherwise.
 * Idempotent. Transports start after this — espos_wifi_start() refuses to run
 * before it — so the hostname is set on every interface they create.
 */
esp_err_t espos_net_start(void);

/** Snapshot of the current status (thread-safe copy; never calls a driver).
 * ESP_ERR_INVALID_STATE before espos_net_start(). */
esp_err_t espos_net_get_status(espos_net_status_t *out);

/** True while a default route exists. False before espos_net_start(). */
bool espos_net_is_up(void);

/**
 * Called on every default-route change: up, down, or moved to another
 * interface or address (the last is reported to ESPOS_EVENT as NETWORK_DOWN
 * followed by NETWORK_UP, so the "one DOWN per UP" rule holds). Runs on the
 * task of the transport that reported the change — for WiFi the espos_wifi
 * task — with no espos_net lock held; `st` is valid for the call
 * only. Copy it out and return; never block. `arg` is handed back untouched.
 * Small fixed table: ESP_ERR_NO_MEM when full; the same (cb, arg) pair
 * registered twice is called once. Callable before espos_net_start().
 */
typedef void (*espos_net_cb_t)(const espos_net_status_t *st, void *arg);
esp_err_t espos_net_subscribe(espos_net_cb_t cb, void *arg);
esp_err_t espos_net_unsubscribe(espos_net_cb_t cb, void *arg);

/**
 * The User-Agent espOS sends to the SignalK server: "espOS/\<espOS version\>
 * \<app\>/\<app version\>". Each client sets it (the delta stream, espos_sk_http,
 * the BLE gateway's control socket, OTA); a client that leaves it unset sends
 * IDF's default, "ESP32 Websocket Client" or "ESP32 HTTP Client/1.0", which a
 * server cannot tell from SensESP or any other IDF firmware.
 */
const char *espos_net_user_agent(void);

/**
 * Device-unique short id, "1a2b": the last two bytes of the base MAC read from
 * eFuse, so it is the same whether the device speaks WiFi, Ethernet or Thread,
 * and the same on the ESP32-P4 whose radio is a co-processor. Default names
 * (hostname espos-<id>, portal SSID espOS-<id>, the SignalK source label) all
 * derive from it. On the linux target a fixed "1a2b". "" before
 * espos_net_start().
 */
const char *espos_net_short_id(void);

/**
 * Reconnect backoff for round `round`: 1 s · 2^round, capped at cap_ms, ±25 %
 * jitter from `rnd`, never below 250 ms. The same curve every espOS retry
 * loop uses (WiFi rounds, the SignalK stream, HTTP retries), moved here from
 * espos_wifi so a loop needs no radio to back off. Pure function.
 */
uint32_t espos_net_backoff_ms(uint32_t round, uint32_t cap_ms, uint32_t rnd);

/** The status as the JSON document of docs/rest-api.md (malloc'ed; caller frees). */
esp_err_t espos_net_status_json(char **out_json);

/** Name of an interface as the REST document spells it: "none", "wifi_sta", "eth", "thread". */
const char *espos_net_if_str(espos_net_if_t iface);

/* ------------------------------------------------ for transports to call */

/**
 * A transport hands over the esp_netif it drives (esp_netif_t *, opaque here
 * so this header carries no IDF type). espos_net sets the hostname on it and
 * reads its link-local IPv6 address for the status. Callable before or after
 * espos_net_start(); ESP_ERR_INVALID_ARG for NONE or an out-of-range iface.
 * The host build has no netif: pass NULL, nothing is applied.
 */
esp_err_t espos_net_register_if(espos_net_if_t iface, void *esp_netif);

/**
 * A transport reports its link: `up` with the dotted addresses (gateway may
 * be "" or NULL) and, for WiFi, the RSSI; or `up == false` and the rest
 * ignored. Report on every change — got an address, lost it, refreshed the
 * RSSI. Identical reports are cheap no-ops. espos_net decides whether the
 * default route changed and, if so, posts NETWORK_UP / NETWORK_DOWN and runs
 * the subscribers on the caller's task before returning. Thread-safe;
 * ignored before espos_net_start().
 */
void espos_net_report(espos_net_if_t iface, bool up, const char *ip, const char *netmask, const char *gateway, int8_t rssi);

#ifdef __cplusplus
}
#endif
