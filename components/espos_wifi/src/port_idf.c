/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device port: esp_wifi / esp_netif / esp_event glue for the state
 * machine. On ESP32-P4 the same calls reach the C6 co-processor through
 * esp_wifi_remote; nothing here knows the difference.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_event.h"
#include "wifi_last_ap.h"

#include "esp_attr.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_net.h"
#include "espos_wifi_priv.h"
#include "wifi_fresh.h"
#include "wifi_ps.h"

static const char *TAG = "espos_wifi";

#define PORTAL_IP "192.168.4.1"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static TimerHandle_t s_timer;
static bool s_disconnect_requested;   /* our own esp_wifi_disconnect() is in flight */
static bool s_portal_up;
static int s_portal_clients;
static bool s_inited;
static char s_ssid[33];     /* of the current association, for the GOT_IP line */
static espos_wifi_fresh_t s_fresh; /* wifi.fresh_assoc */
/* Cached rather than read in the event handler: the getter waits on the config
 * store lock, which an import holds across all its NVS writes, and the event
 * loop must not stall behind that. */
static atomic_bool s_fresh_enabled;
/* Bumped by every connect or disconnect the state machine issues. The drop's
 * echo reconnects only if this is unchanged since the drop: otherwise the state
 * machine has taken the radio back (or let it go) in between, from another task,
 * and a reconnect would act behind its back. */
static atomic_uint s_sm_radio_gen;
static atomic_uint s_fresh_gen;

/* Last AP we actually associated with, remembered across a disconnect so the
 * next attempts can go straight to it instead of scanning every channel first.
 *
 * WHY IT IS WORTH THE COMPLEXITY: a WIFI_ALL_CHANNEL_SCAN costs ~2.3 s before
 * `state: init -> auth` (measured on an ESP32-S3, espOS #136), and without a
 * remembered channel every attempt pays it -- including a reconnect to the AP the
 * device was just talking to. Against an AP that needs a few tries to complete
 * the handshake, that is 2.3 s added to each of them, which is what makes a
 * 30-60 s cold join out of a 2 s one.
 *
 * This is a CACHE, not a pin. espos_wifi_net_t.has_bssid is the pin -- an
 * operator saying "only ever this BSSID" -- and it must keep meaning that, so it
 * takes precedence and is never overwritten from here.
 *
 * The cache is allowed to be wrong, and a wrong channel costs time rather than an
 * attempt: wifi_sta_config_t.channel is documented as a HINT -- "scan starting
 * from the specified channel" -- so the driver orders its scan and still finds
 * the AP wherever it actually is. Measured downstream on hardware (espOS #136):
 * with the cache naming channel 1 and the AP moved to 6 and then to 11, both
 * connected on the FIRST attempt in 0.81-1.64 s, still under the ~2.41 s a full
 * scan costs. So the attempt budget below is not the mechanism that recovers a
 * moved AP -- the driver does that by itself -- it bounds how long a cache that
 * has gone stale for some OTHER reason (the AP replaced, the device carried to a
 * different one on the same SSID) keeps being preferred.
 *
 * Kept per SSID: reconnecting to a DIFFERENT network in the list must not reuse
 * another network's BSSID, which would pin the wrong AP entirely.
 *
 * RTC_NOINIT rather than plain static, because the reported case includes a
 * FRESH BOOT: a device that reboots -- an OTA, a watchdog, a power blip -- would
 * otherwise start with an empty cache and pay the full scan on exactly the
 * attempts this is meant to shorten. RTC memory survives a reset (and deep
 * sleep) but holds garbage after a cold power-on, so the magic and the checksum
 * are what tell "remembered" from "never written", the same pattern
 * espos_power/src/port_idf.c uses. A NVS write per association would be the
 * alternative and is worse: this changes on every roam and NVS has a finite
 * erase budget. */
static RTC_NOINIT_ATTR espos_wifi_last_ap_t s_last_ap;

/* wifiStalled's "once per outage" (espos_wifi.c). RTC_NOINIT for the same
 * reason as s_last_ap, and because the health reset record is not enough: it
 * is gone after the next reset of any kind, so a co-processor watchdog restart
 * or an OTA during the held-off boot would re-arm the restart and start the
 * loop it exists to prevent. Garbage after a power-on reads as "not latched"
 * unless it happens to equal the magic, and a power-on is a fresh start anyway. */
static RTC_NOINIT_ATTR uint32_t s_stall_latch;

/* How many attempts may use the cached BSSID/channel before falling back to a
 * full scan. Small on purpose: the point is to remove the scan from the retries
 * that follow a handshake miss against an AP that is definitely there, not to
 * keep chasing an AP that has gone. Three covers the 1-3 misses reported in #136
 * while costing at most three fast failures when the AP really has moved. */
#define FAST_RECONNECT_ATTEMPTS 3

/* Fast attempts still available before falling back to a full scan. Refilled when
 * a connection actually WORKS (GOT_IP), so a device that keeps reconnecting to a
 * live AP keeps the shortcut, while one whose AP has gone -- or associates but
 * never leases an address -- spends the budget once and then scans properly.
 *
 * Initialised to the full budget, not 0: after a reboot the remembered AP is in
 * RTC memory but this counter is not, and starting empty would switch the cache
 * off for exactly the fresh-boot case it exists to help. A cold power-on gets the
 * budget too, but then espos_wifi_last_ap_usable() rejects the uninitialised record
 * and nothing is spent. */
static uint8_t s_fast_attempts_left = FAST_RECONNECT_ATTEMPTS;

esp_err_t espos_wifi_portal_dns_start(const char *ip);
void espos_wifi_portal_dns_stop(void);

/* ----------------------------------------------------------- events */

/* Straight back to the AP we just left, on its channel, without a scan. The
 * state machine is still in CONNECTING with its connect timeout running, so if
 * this fails the disconnect or the timeout reaches it as a failed attempt and
 * its next p_connect() writes the full configuration again. Run by the drainer
 * (espos_wifi.c), so no connect or disconnect of the state machine's can slip
 * in between the generation check and esp_wifi_connect(), and the blocking
 * driver calls stay where the state machine's own do. */
static esp_err_t d_fresh_reconnect(void)
{
    if (atomic_load(&s_sm_radio_gen) != atomic_load(&s_fresh_gen)) {
        return ESP_OK; /* the state machine has taken the radio since the drop */
    }
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && s_last_ap.channel) {
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, s_last_ap.bssid, 6);
        cfg.sta.channel = s_last_ap.channel;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (cfg_err != ESP_OK) {
            /* the connect still runs, with a scan instead of the pinned AP */
            ESP_LOGW(TAG, "esp_wifi_set_config: %s", esp_err_to_name(cfg_err));
        }
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    return err;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = data;
        espos_wifi_link_t link = { 0 };
        size_t n = e->ssid_len < 32 ? e->ssid_len : 32;
        memcpy(link.ssid, e->ssid, n);
        memcpy(link.bssid, e->bssid, 6);
        link.channel = e->channel;
        memcpy(s_ssid, link.ssid, sizeof(s_ssid));

        /* Remember where we got in. Recorded on ASSOCIATION rather than on
         * GOT_IP deliberately: the scan this saves happens before auth, so an
         * association that succeeds and then fails DHCP still tells us which
         * channel this AP is on. */
        /* Store the AP here, because association is where the channel comes
         * from and an association that then fails DHCP still tells us which
         * channel this AP is on. The BUDGET is refilled in the GOT_IP handler
         * instead: refilling it here would mean an AP that associates but never
         * leases an address -- a broken DHCP server, a VLAN with no pool --
         * tops the budget up on every attempt, so the device retries the fast
         * path forever and never falls back to the full scan that might find a
         * usable AP. Associating is not the same as working. */
        espos_wifi_last_ap_store(&s_last_ap, link.ssid, e->bssid, e->channel);
        /* The state machine is not told about this association (wifi_fresh.c). */
        if (espos_wifi_fresh_pending(&s_fresh)) {
            /* The replacement (or, if the echo was lost, the next) association:
             * this one is reported, and its address must not be swallowed as
             * the dropped one's. */
            (void)espos_wifi_fresh_on_connected(&s_fresh, false);
        } else if (espos_wifi_fresh_armed(&s_fresh)) {
            atomic_store(&s_fresh_gen, atomic_load(&s_sm_radio_gen));
            if (espos_wifi_fresh_on_connected(&s_fresh, atomic_load(&s_fresh_enabled))) {
                ESP_LOGI(TAG, "dropping the first association so the access point starts a fresh one");
                if (esp_wifi_disconnect() == ESP_OK) {
                    break;
                }
                espos_wifi_fresh_cancel(&s_fresh);
            }
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            link.rssi = ap.rssi; /* we are on the event task, not under the SM lock */
        }
        espos_wifi_dispatch(ESPOS_WIFI_EV_STA_CONNECTED, &link);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = data;
        int reason = e->reason;
        if (espos_wifi_fresh_on_disconnected(&s_fresh, reason == WIFI_REASON_ASSOC_LEAVE)) {
            /* Checked before s_disconnect_requested, which a disconnect of the
             * state machine's that never echoed can leave set: the drop's echo
             * would be swallowed as that one's and nothing would reconnect. */
            s_disconnect_requested = false;
            espos_wifi_request_fresh_reconnect();
            break;
        }
        if (s_disconnect_requested && reason == WIFI_REASON_ASSOC_LEAVE) {
            /* the echo of our own esp_wifi_disconnect(); the SM already moved on */
            s_disconnect_requested = false;
            ESP_LOGD(TAG, "swallowing our own disconnect echo");
            break;
        }
        ESP_LOGW(TAG, "disconnected: %d (%s)", reason, espos_wifi_reason_str(reason));
        espos_wifi_dispatch(ESPOS_WIFI_EV_STA_DISCONNECTED, &reason);
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        const wifi_event_sta_scan_done_t *e = data;
        if (e && e->status != 0) {
            ESP_LOGW(TAG, "scan failed (status %" PRIu32 ")", (uint32_t)e->status);
            esp_wifi_clear_ap_list();
            espos_wifi_scan_done(NULL, 0);
            break;
        }
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 20) {
            n = 20;
        }
        /* heap, not stack: this runs on the (small) system event task */
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        espos_wifi_scan_entry_t *out = calloc(n ? n : 1, sizeof(espos_wifi_scan_entry_t));
        size_t count = 0;
        if (recs && out && n) {
            uint16_t got = n;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) { /* also frees the driver list */
                for (uint16_t i = 0; i < got && count < n; i++) {
                    espos_wifi_scan_entry_t *o = &out[count++];
                    strncpy(o->ssid, (const char *)recs[i].ssid, 32);
                    memcpy(o->bssid, recs[i].bssid, 6);
                    o->rssi = recs[i].rssi;
                    o->channel = recs[i].primary;
                    o->authmode = (uint8_t)recs[i].authmode;
                }
            }
        } else {
            esp_wifi_clear_ap_list();
        }
        espos_wifi_scan_done(out, count);
        free(out);
        free(recs);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        s_portal_clients++;
        espos_wifi_dispatch(ESPOS_WIFI_EV_PORTAL_CLIENT, &s_portal_clients);
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_portal_clients > 0) {
            s_portal_clients--;
        }
        espos_wifi_dispatch(ESPOS_WIFI_EV_PORTAL_CLIENT, &s_portal_clients);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        if (espos_wifi_fresh_pending(&s_fresh)) {
            /* A static address is "leased" on association, the dropped one
             * included: neither a connection nor a reason to refill the budget. */
            return;
        }
        /* A usable connection, not merely an association: this is what earns
         * the fast-reconnect budget back. See the STA_CONNECTED handler for why
         * it is not refilled there. */
        s_fast_attempts_left = FAST_RECONNECT_ATTEMPTS;
        const ip_event_got_ip_t *e = data;
        espos_wifi_ip_t ip;
        snprintf(ip.ip, sizeof(ip.ip), IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(ip.netmask, sizeof(ip.netmask), IPSTR, IP2STR(&e->ip_info.netmask));
        snprintf(ip.gateway, sizeof(ip.gateway), IPSTR, IP2STR(&e->ip_info.gw));
        const char *hostname = NULL;
        if (!s_sta_netif || esp_netif_get_hostname(s_sta_netif, &hostname) != ESP_OK || !hostname) {
            hostname = "espos";
        }
        ESP_LOGI(TAG, "connected to \"%s\" as %s — web UI: http://%s.local", s_ssid, ip.ip, hostname);
        /* The machine reports the new link to espos_net from its drainer,
         * still inside this handler, so by the time NETWORK_UP is posted both
         * espos_wifi_get_status() and espos_net_get_status() already say up. */
        espos_wifi_dispatch(ESPOS_WIFI_EV_GOT_IP, &ip);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost ip");
        espos_wifi_dispatch(ESPOS_WIFI_EV_LOST_IP, NULL);
    } else if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        /* The portal's DHCP server handing out a lease. Without this the log
         * shows a client associating and then nothing, which reads as "the
         * DHCP server is down" whatever the cause actually was -- the two
         * states that need opposite fixes (the radio associated but the lease
         * never happened, versus both worked and the browser never opened the
         * page) are indistinguishable. */
        const ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "portal: lease " IPSTR " to %02x:%02x:%02x:%02x:%02x:%02x", IP2STR(&e->ip),
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    }
}

/* ---------------------------------------------------------- SM port */

static bool parse_ip4(const char *str, esp_ip4_addr_t *out)
{
    return str && str[0] && esp_netif_str_to_ip4(str, out) == ESP_OK;
}

/* wifi.ip_mode and friends, applied to the station netif before every
 * connect so a change takes effect at the next (re)connection. Static
 * addressing still raises IP_EVENT_STA_GOT_IP on association (esp_netif
 * reports the configured address when its DHCP client is stopped), so the
 * state machine sees exactly what it sees with DHCP. */
static void apply_ip_mode(void)
{
    char mode[8] = "dhcp";
    espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_IP_MODE, mode, sizeof(mode), NULL);
    char ip[16] = { 0 }, mask[16] = { 0 }, gw[16] = { 0 }, dns0[16] = { 0 }, dns1[16] = { 0 };
    esp_netif_ip_info_t info = { 0 };
    bool static_ok = false;
    if (strcmp(mode, "static") == 0) {
        espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_IP, ip, sizeof(ip), NULL);
        espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_NETMASK, mask, sizeof(mask), NULL);
        espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_GATEWAY, gw, sizeof(gw), NULL);
        espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_DNS0, dns0, sizeof(dns0), NULL);
        espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_DNS1, dns1, sizeof(dns1), NULL);
        static_ok = parse_ip4(ip, &info.ip) && parse_ip4(mask, &info.netmask);
        if (!static_ok) {
            ESP_LOGW(TAG, "wifi.ip_mode is static but ip \"%s\" / netmask \"%s\" is not an address: using DHCP", ip, mask);
        }
        (void)parse_ip4(gw, &info.gw); /* optional; 0.0.0.0 = no gateway */
    }
    if (!static_ok) {
        /* DHCP, or back to it: a start on a running client is ALREADY_STARTED, harmless */
        esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcpc_start: %s", esp_err_to_name(err));
        }
        return;
    }
    esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(err));
    }
    err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ip_info: %s", esp_err_to_name(err));
    }
    /* Without a DHCP lease nobody hands out resolvers: dns0 falls back to the
     * gateway, which on a boat is the router that resolves anyway. */
    const char *dns[2] = { dns0[0] ? dns0 : gw, dns1 };
    const esp_netif_dns_type_t types[2] = { ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP };
    for (int i = 0; i < 2; i++) {
        esp_netif_dns_info_t d = { 0 };
        if (parse_ip4(dns[i], &d.ip.u_addr.ip4)) {
            d.ip.type = ESP_IPADDR_TYPE_V4;
            (void)esp_netif_set_dns_info(s_sta_netif, types[i], &d);
        }
    }
    ESP_LOGI(TAG, "static address %s/%s gateway %s dns %s%s%s", ip, mask, gw[0] ? gw : "none", dns[0][0] ? dns[0] : "none",
             dns1[0] ? " " : "", dns1);
}

static esp_err_t p_connect(void *ctx, const espos_wifi_net_t *net)
{
    (void)ctx;
    /* NB: s_disconnect_requested is deliberately left as is: the echo of a
     * disconnect issued just before this connect (same drain) still has to
     * be swallowed when it arrives during the new attempt. */
    atomic_fetch_add(&s_sm_radio_gen, 1);
    apply_ip_mode();
    wifi_config_t cfg = { 0 };
    /* wifi_sta_config_t: ssid[32] / password[64] need not be NUL-terminated
     * when full; strncpy pads shorter values with NUL. */
    strncpy((char *)cfg.sta.ssid, net->ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, net->psk, sizeof(cfg.sta.password));
    bool fast = false;
    if (net->has_bssid) {
        /* The operator's pin wins, always. It means "only this BSSID", so the
         * cache must not widen or narrow it. */
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, net->bssid, 6);
    } else if (espos_wifi_last_ap_usable(&s_last_ap, net->ssid, s_fast_attempts_left,
                                         net->has_bssid)) {
        /* Go straight back to the AP we were last associated with on this SSID.
         * Channel AND bssid: the channel is what removes the scan, the bssid is
         * what stops WIFI_FAST_SCAN settling for a different AP in the same ESS
         * on the way past. */
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, s_last_ap.bssid, 6);
        cfg.sta.channel = s_last_ap.channel;
        fast = true;
        s_fast_attempts_left--;
    }
    /* FAST_SCAN stops at the first acceptable AP, which is the whole point when
     * we already know which one that is. Otherwise scan everything and take the
     * strongest -- the behaviour every other build has had. */
    cfg.sta.scan_method = fast ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* With a password configured refuse open/WEP networks (WPA-PSK is the
     * floor; WPA2 and WPA3-SAE/H2E are negotiated when the AP offers them). */
    cfg.sta.threshold.authmode = net->psk[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
        return err;
    }
    if (net->has_bssid) {
        ESP_LOGI(TAG, "connecting to '%s' (pinned BSSID)", net->ssid);
    } else if (fast) {
        /* Say so: an operator comparing a slow cold boot with a fast reconnect
         * needs to know which of the two paths a given attempt took. */
        ESP_LOGI(TAG, "connecting to '%s' (channel %u, last known AP)", net->ssid,
                 (unsigned)s_last_ap.channel);
    } else {
        ESP_LOGI(TAG, "connecting to '%s' (all channels)", net->ssid);
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t p_disconnect(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&s_sm_radio_gen, 1);
    s_disconnect_requested = true;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        s_disconnect_requested = false;
    }
    return ESP_OK;
}

static esp_err_t p_portal_start(void *ctx)
{
    (void)ctx;
    if (s_portal_up) {
        return ESP_OK;
    }
    char ssid[33], psk[65];
    espos_wifi_portal_credentials(ssid, psk);
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100; /* explicit: a hosted co-processor does not default 0 → 100 */
    if (psk[0]) {
        strncpy((char *)ap.ap.password, psk, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.pmf_cfg.capable = true;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_LOGD(TAG, "portal: switching to APSTA");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGD(TAG, "portal: set_mode → %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        ESP_LOGD(TAG, "portal: set_config → %s", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portal start failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }
    /* esp_wifi_set_mode(APSTA) brings the access point up carrying the
     * driver's default configuration, and the set_config above then applies
     * ours -- which restarts it. IDF starts the DHCP server off the netif's
     * up-event, so on a restart it can end up bound while the interface is
     * down, and a client then associates but never gets a lease. Observed on
     * the P4: "softap started / stopped", DHCP started, "softap started".
     *
     * Configuring before the mode is not an option -- esp_wifi_set_config()
     * answers ESP_ERR_WIFI_MODE while the current mode has no AP -- so make
     * sure the server is running once the interface has settled instead. */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        /* This runs while the interface is still coming up, so the server is
         * normally not started yet and we start it here; IDF starting it again
         * on the netif's up-event is harmless. What this removes is the case
         * where the restart leaves nothing bound and a client gets no lease. */
        esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
        if (esp_netif_dhcps_get_status(ap_netif, &st) == ESP_OK && st != ESP_NETIF_DHCP_STARTED) {
            esp_err_t derr = esp_netif_dhcps_start(ap_netif);
            if (derr != ESP_OK && derr != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGE(TAG, "portal: dhcps_start: %s (clients will not get an address)",
                         esp_err_to_name(derr));
            }
        }
    }

    s_portal_clients = 0;
    s_portal_up = true;
    espos_wifi_portal_dns_start(PORTAL_IP);
    ESP_LOGI(TAG, "portal up: join \"%s\" (%s) and open http://%s/", ssid, psk[0] ? "WPA2" : "open", PORTAL_IP);
    return ESP_OK;
}

static esp_err_t p_portal_stop(void *ctx)
{
    (void)ctx;
    if (!s_portal_up) {
        return ESP_OK;
    }
    espos_wifi_portal_dns_stop();
    s_portal_up = false;
    s_portal_clients = 0;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "portal down");
    return err;
}

static void timer_cb(TimerHandle_t t)
{
    (void)t;
    espos_wifi_dispatch(ESPOS_WIFI_EV_TIMER, NULL);
}

/* Timer commands go through the daemon's queue; from the daemon task itself
 * we must not block, elsewhere a short wait beats silently losing a state
 * timeout when the queue is momentarily full. */
static TickType_t timer_block(void)
{
    return xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle() ? 0 : pdMS_TO_TICKS(100);
}

static void p_arm_timer(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (!s_timer) {
        return;
    }
    /* round UP: the SM ignores a fire that arrives before now_ms()+ms */
    TickType_t ticks = (ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1;
    if (xTimerChangePeriod(s_timer, ticks, timer_block()) != pdPASS) { /* also starts it */
        ESP_LOGE(TAG, "timer re-arm failed (queue full)");
    }
}

static void p_cancel_timer(void *ctx)
{
    (void)ctx;
    if (s_timer && xTimerStop(s_timer, timer_block()) != pdPASS) {
        ESP_LOGE(TAG, "timer stop failed (queue full)");
    }
}

static uint32_t p_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t p_random(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static const espos_wifi_port_t k_sm_port = {
    .connect = p_connect,
    .disconnect = p_disconnect,
    .portal_start = p_portal_start,
    .portal_stop = p_portal_stop,
    .arm_timer = p_arm_timer,
    .cancel_timer = p_cancel_timer,
    .now_ms = p_now_ms,
    .random = p_random,
    .status_changed = NULL, /* filled by the core */
};

/* ---------------------------------------------------------- driver */

static void load_fresh(void)
{
    bool v = false;
    espos_config_get_bool(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_FRESH_ASSOC, &v);
    atomic_store(&s_fresh_enabled, v);
}

/* On the writer's task, so a change made before the first association (from
 * the setup portal, say) still applies to it. */
static void on_fresh_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (strcmp(ns, ESPOS_CFG_NS_WIFI) == 0 && strcmp(key, ESPOS_CFG_WIFI_FRESH_ASSOC) == 0) {
        load_fresh();
    }
}

static esp_err_t d_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    /* espos_net sets the hostname on it now (before the first DHCP request)
     * and reads its link-local address for /net/status. */
    (void)espos_net_register_if(ESPOS_NET_IF_WIFI_STA, s_sta_netif);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Credentials live in espos_config; do not let the driver persist its own copy. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    static bool handlers_registered;
    if (!handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_ip_event, NULL));
        (void)espos_config_subscribe(on_fresh_change, NULL);
        handlers_registered = true;
    }
    load_fresh();
    if (!s_timer) {
        s_timer = xTimerCreate("espos_wifi", pdMS_TO_TICKS(1000), pdFALSE, NULL, timer_cb);
        if (!s_timer) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
#if ESPOS_WIFI_I154_COEX
    /* A Zigbee or Thread build shares the chip's one 2.4 GHz radio with
     * 802.15.4, and the software arbiter schedules the two only once it is
     * told to; Bluetooth gets that by itself, 802.15.4 does not. Without it a
     * coordinator, whose receiver is always on, starves the station: the AP
     * drops it for missing ACKs and every rejoin times out. */
    err = esp_coex_wifi_i154_enable();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "802.15.4 coexistence not enabled: %s", esp_err_to_name(err));
    }
#endif
    s_inited = true;
    return ESP_OK;
}

static esp_err_t d_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    p_portal_stop(NULL);
    esp_wifi_stop();
    esp_wifi_deinit(); /* d_init re-inits; netifs, handlers and the timer stay */
    s_inited = false;
    return ESP_OK;
}

static int8_t d_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static esp_err_t d_scan_start(void)
{
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_ERR_WIFI_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static esp_err_t d_set_ps(const char *mode)
{
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (strcmp(mode, "min") == 0) {
        ps = WIFI_PS_MIN_MODEM;
    } else if (strcmp(mode, "max") == 0) {
        ps = WIFI_PS_MAX_MODEM;
    }
    return esp_wifi_set_ps(ps);
}

static const espos_wifi_driver_t k_driver = {
    .init = d_init,
    .deinit = d_deinit,
    .sm_port = &k_sm_port,
    .rssi = d_rssi,
    .scan_start = d_scan_start,
    .set_ps = d_set_ps,
    .portal_ip = PORTAL_IP,
    .stall_latch = &s_stall_latch,
    .fresh_reconnect = d_fresh_reconnect,
};

const espos_wifi_driver_t *espos_wifi_driver(void)
{
    return &k_driver;
}
