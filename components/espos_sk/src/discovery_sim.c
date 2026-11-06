/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stand-in for mDNS discovery: servers come from the environment,
 *   ESPOS_SIM_SK_SERVERS="host,port,self,name[,tls];host,port,self,name[,tls]"
 * (self/name/tls optional; URNs contain colons, hence commas). A fifth field
 * of "tls" or "1" is the entry advertising itself as _signalk-https._tcp,
 * which is what sk.scheme = auto reads on a device. Empty or unset = nothing
 * discovered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espos_sk_priv.h"

esp_err_t espos_sk_discovery_init(const char *hostname)
{
    (void)hostname;
    return ESP_OK;
}

size_t espos_sk_discovery_run(espos_sk_discovered_t *out, size_t max, const espos_sk_keep_t *keep)
{
    const char *env = getenv("ESPOS_SIM_SK_SERVERS");
    if (!env || !*env) {
        return 0;
    }
    char *copy = strdup(env);
    if (!copy) {
        return 0;
    }
    size_t n = 0;
    char *save = NULL;
    /* Every entry, through the same rule as real discovery: the anchored
     * server gets in on a full list. */
    for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        espos_sk_discovered_t entry;
        espos_sk_discovered_t *d = &entry;
        memset(d, 0, sizeof(*d));
        char *f = tok;
        char *host = strsep(&f, ",");
        char *port = f ? strsep(&f, ",") : NULL;
        char *self = f ? strsep(&f, ",") : NULL;
        char *name = f ? strsep(&f, ",") : NULL;
        char *tls = f;
        if (!host || !port) {
            continue;
        }
        snprintf(d->host, sizeof(d->host), "%s", host);
        d->port = (uint16_t)atoi(port);
        if (self) {
            snprintf(d->self, sizeof(d->self), "%s", self);
        }
        d->tls = tls && (strcmp(tls, "tls") == 0 || strcmp(tls, "1") == 0);
        snprintf(d->name, sizeof(d->name), "%s", name ? name : "sim");
        snprintf(d->roles, sizeof(d->roles), "master, main");
        snprintf(d->swname, sizeof(d->swname), "signalk-server");
        snprintf(d->swvers, sizeof(d->swvers), "sim");
        espos_sk_disc_add(out, &n, max, d, keep);
    }
    free(copy);
    return n;
}
