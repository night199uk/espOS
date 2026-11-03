// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
/*
 * wifi.fresh_assoc (espOS #289).
 *
 * A device that restarts without deauthenticating -- a power cut, a panic, a
 * flash that resets the chip -- leaves the access point holding its station.
 * Most APs replace that state when the station authenticates again. A MikroTik
 * hAP ax S (RouterOS 7.24) does not: it logs "reconnecting", the station gets
 * its lease and its broadcasts, and every unicast frame to it is lost until the
 * AP ages the entry out minutes later. A deauthentication clears it, so with
 * the setting on the first association of each boot is dropped once and made
 * again, which costs about two seconds per boot.
 *
 * The state machine never sees the dropped association: it is still waiting in
 * CONNECTING, and the second association is the one it is told about. Split
 * out of port_idf.c so the once-per-boot rule and the echo handling are
 * host-tested; getting either wrong is a device that drops every connection or
 * reconnects behind the state machine's back.
 */

#include "wifi_fresh.h"

#include <stddef.h>

enum {
    FRESH_ARMED = 0, /* the first association of this boot is still to come */
    FRESH_PENDING,   /* dropped, waiting for the echo of our disconnect */
    FRESH_REJOINING, /* echo seen, the replacement association is still to come */
    FRESH_DONE,
};

bool espos_wifi_fresh_armed(const espos_wifi_fresh_t *f)
{
    return f && atomic_load(&f->phase) == FRESH_ARMED;
}

bool espos_wifi_fresh_pending(const espos_wifi_fresh_t *f)
{
    if (!f) {
        return false;
    }
    uint_fast8_t p = atomic_load(&f->phase);
    return p == FRESH_PENDING || p == FRESH_REJOINING;
}

bool espos_wifi_fresh_on_connected(espos_wifi_fresh_t *f, bool enabled)
{
    if (!f) {
        return false;
    }
    uint_fast8_t armed = FRESH_ARMED;
    if (enabled && atomic_compare_exchange_strong(&f->phase, &armed, FRESH_PENDING)) {
        return true;
    }
    /* Disabled, already done, or an association while the drop is pending (the
     * echo was lost): report it, and never drop a second one. */
    atomic_store(&f->phase, FRESH_DONE);
    return false;
}

bool espos_wifi_fresh_on_disconnected(espos_wifi_fresh_t *f, bool leave)
{
    if (!f) {
        return false;
    }
    /* Still pending past the echo, so an address the dropped association's
     * DHCP delivers late is not taken for the replacement's. */
    uint_fast8_t pending = FRESH_PENDING;
    if (leave && atomic_compare_exchange_strong(&f->phase, &pending, FRESH_REJOINING)) {
        return true;
    }
    /* Any other reason while pending, or the rejoin failing: the state machine's. */
    espos_wifi_fresh_cancel(f);
    return false;
}

void espos_wifi_fresh_cancel(espos_wifi_fresh_t *f)
{
    if (!f) {
        return;
    }
    uint_fast8_t p = FRESH_PENDING;
    if (!atomic_compare_exchange_strong(&f->phase, &p, FRESH_DONE)) {
        p = FRESH_REJOINING;
        (void)atomic_compare_exchange_strong(&f->phase, &p, FRESH_DONE);
    }
}
