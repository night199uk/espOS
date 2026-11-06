/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * mDNS discovery of SignalK servers (_signalk-http._tcp). The responder the
 * queries go through — and the device's own advertisement — is espos_net's
 * (espos_mdns.h); this file only browses. Device build; the host uses
 * discovery_sim.c.
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_mdns.h"
#include "espos_sk_priv.h"

static const char *TAG = "espos_sk";

#if CONFIG_ESPOS_NET_MDNS

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mdns.h"

#include "espos_net.h"

/* Our own IPv4 + netmask on the default route (0 when down). */
static void local_net(uint32_t *ip, uint32_t *mask)
{
    *ip = 0;
    *mask = 0;
    espos_net_status_t ns;
    if (espos_net_get_status(&ns) == ESP_OK && ns.up) {
        *ip = ipaddr_addr(ns.ip);
        *mask = ipaddr_addr(ns.netmask);
    }
}

esp_err_t espos_sk_discovery_init(const char *hostname)
{
    (void)hostname; /* the responder names itself after net.hostname */
    /* The responder is espos_net's and espos_net_start() brings it up;
     * asking again here costs nothing (idempotent) and keeps a SignalK build
     * findable when a firmware drives the start sequence by hand. */
    return espos_mdns_start();
}

/* espos_net reports the route up from inside the transport's GOT_IP handler
 * while the responder's MDNS_READY is still queued behind it on the event
 * loop, so the browse the sk task fires on that edge can arrive a few
 * milliseconds early.
 * An empty pass is only retried after a whole sk.discover_s; wait for the
 * responder instead — bounded, and only while there is a link to wait for. */
static bool wait_ready(void)
{
    for (int i = 0; i < 60; i++) {
        if (espos_mdns_is_ready()) {
            return true;
        }
        uint32_t ip, mask;
        local_net(&ip, &mask);
        if (!ip) {
            return false; /* no link: nothing to browse on */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return espos_mdns_is_ready();
}

static const char *txt_get(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value ? r->txt[i].value : "";
        }
    }
    return "";
}

/* One browse of one service type, appending to out[] from index `n`. */
static size_t browse(const char *service, bool tls, espos_sk_discovered_t *out, size_t n, size_t max,
                     const espos_sk_keep_t *keep, uint32_t my_ip, uint32_t my_mask)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(service, "_tcp", 3000, 20, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns query %s: %s", service, esp_err_to_name(err));
        return n;
    }
    /* Every answer is read, even with the list full: the anchored server may
     * be among the last (espos_sk_disc_add). */
    for (mdns_result_t *r = results; r; r = r->next) {
        espos_sk_discovered_t entry;
        espos_sk_discovered_t *d = &entry;
        memset(d, 0, sizeof(*d));
        d->tls = tls;
        /* Dual-homed servers advertise several A records; prefer the one on
         * our own subnet, else the first IPv4, else <hostname>.local. */
        bool have_ip = false;
        for (int pass = 0; pass < 2 && !have_ip; pass++) {
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                    continue;
                }
                uint32_t ip = a->addr.u_addr.ip4.addr;
                bool same_net = my_mask && ((ip & my_mask) == (my_ip & my_mask));
                if (pass == 0 && !same_net) {
                    continue;
                }
                snprintf(d->host, sizeof(d->host), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                have_ip = true;
                break;
            }
        }
        if (!have_ip) {
            if (!r->hostname) {
                continue;
            }
            snprintf(d->host, sizeof(d->host), "%s.local", r->hostname);
        }
        d->port = r->port;
        snprintf(d->name, sizeof(d->name), "%s", r->instance_name ? r->instance_name : "");
        snprintf(d->self, sizeof(d->self), "%s", txt_get(r, "self"));
        snprintf(d->roles, sizeof(d->roles), "%s", txt_get(r, "roles"));
        snprintf(d->swname, sizeof(d->swname), "%s", txt_get(r, "swname"));
        snprintf(d->swvers, sizeof(d->swvers), "%s", txt_get(r, "swvers"));
        /* A server that advertises both types (it should not, but a stale
         * record outlives a config change) is listed once, at the scheme it
         * most recently claimed -- and https comes second here, so the TLS
         * answer wins. Duplicates confuse the election, not the scheme. */
        espos_sk_disc_add(out, &n, max, d, keep);
    }
    mdns_query_results_free(results);
    return n;
}

size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max, const espos_sk_keep_t *keep)
{
    if (!wait_ready()) {
        return 0;
    }
    uint32_t my_ip, my_mask;
    local_net(&my_ip, &my_mask);
    /* Both service types, because that is how signalk-server says which
     * scheme it speaks: `ssl: true` publishes _signalk-https._tcp instead of
     * _signalk-http._tcp (src/interfaces/rest.js). A device that only browsed
     * the plaintext type could not see a TLS-only server at all -- which is
     * precisely the server this whole feature exists for. */
    size_t n = browse("_signalk-http", false, out, 0, max, keep, my_ip, my_mask);
    n = browse("_signalk-https", true, out, n, max, keep, my_ip, my_mask);
    return n;
}

#else /* !CONFIG_ESPOS_NET_MDNS */

/* No responder in this build: there is nothing to browse with. The periodic
 * pass stays (it also ages out stale entries) and finds nothing. */
esp_err_t espos_sk_discovery_init(const char *hostname)
{
    (void)hostname;
    ESP_LOGW(TAG, "built without mDNS (CONFIG_ESPOS_NET_MDNS=n): discovery is off, set sk.server_host");
    return ESP_OK;
}

size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max, const espos_sk_keep_t *keep)
{
    (void)out;
    (void)max;
    (void)keep;
    return 0;
}

#endif /* CONFIG_ESPOS_NET_MDNS */
