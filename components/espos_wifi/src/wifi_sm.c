/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_wifi state machine. Single-threaded by contract: the owner
 * serialises espos_wifi_sm_event() calls (mutex on device, plain calls in
 * tests). One timer, re-armed as needed:
 *
 *   CONNECTING    → connect_timeout        (safety net)
 *   OBTAINING_IP  → dhcp_timeout
 *   BACKOFF       → backoff delay
 *   any, portal   → portal_after (until the portal is up)
 *
 * Portal policy: comes up immediately when nothing is configured (and the
 * portal is enabled), or once we have been without a connection for
 * portal_after; goes down when connected. It runs alongside the station.
 */
#include <string.h>
#include "espos_wifi_sm.h"

#define BASE_BACKOFF_MS 1000u

static void set_state(espos_wifi_sm_t *sm, espos_wifi_state_t s)
{
    if (sm->st.state != s) {
        sm->st.state = s;
    }
}

static void notify(espos_wifi_sm_t *sm)
{
    if (sm->port->status_changed) {
        sm->port->status_changed(sm->port_ctx);
    }
}

static uint32_t now(espos_wifi_sm_t *sm)
{
    return sm->port->now_ms(sm->port_ctx);
}

uint32_t espos_wifi_backoff_ms(uint32_t round, uint32_t cap_ms, uint32_t rnd)
{
    uint64_t d = BASE_BACKOFF_MS;
    for (uint32_t i = 0; i < round && d < cap_ms; i++) {
        d *= 2;
    }
    if (d > cap_ms) {
        d = cap_ms;
    }
    /* ±25 % jitter, never below 250 ms */
    uint32_t span = (uint32_t)(d / 2);              /* 50 % window */
    uint32_t j = span ? (rnd % (span + 1)) : 0;     /* 0..span */
    uint64_t out = d - span / 2 + j;                /* d·0.75 .. d·1.25 */
    if (out < 250) {
        out = 250;
    }
    return (uint32_t)out;
}

/* ---------------------------------------------------------------- portal */

static void portal_up(espos_wifi_sm_t *sm)
{
    if (!sm->st.portal_active && sm->cfg.portal_enabled) {
        if (sm->port->portal_start && sm->port->portal_start(sm->port_ctx) == ESP_OK) {
            sm->st.portal_active = true;
            sm->st.portal_clients = 0;
        }
    }
    sm->portal_due_ms = 0;
}

static void portal_down(espos_wifi_sm_t *sm)
{
    if (sm->st.portal_active) {
        if (sm->port->portal_stop) {
            sm->port->portal_stop(sm->port_ctx);
        }
        sm->st.portal_active = false;
        sm->st.portal_clients = 0;
    }
    sm->portal_due_ms = 0;
}

/* Decide whether the portal should be up now or scheduled. Called on every
 * transition that leaves us without a connection. */
static void portal_policy(espos_wifi_sm_t *sm)
{
    if (!sm->cfg.portal_enabled) {
        portal_down(sm);
        return;
    }
    /* An application that can tell for itself gets the last word, but only
     * inside what the operator allowed above: portal_enabled = false is a
     * configuration choice, not a default to be overridden. */
    if (sm->st.portal_force == ESPOS_WIFI_PORTAL_DOWN) {
        portal_down(sm);
        return;
    }
    if (sm->st.portal_force == ESPOS_WIFI_PORTAL_UP) {
        /* Still not inside an association: switching to APSTA mid-attempt is
         * measurably worse than doing it between attempts (espOS #144), and
         * that holds however the portal was asked for. The request is sticky,
         * so every path out of CONNECTING calls this again and raises it then
         * -- the wait is bounded by the attempt, not by its success. */
        if (sm->connect_in_flight) {
            sm->portal_due_ms = 0;
            return;
        }
        portal_up(sm);
        return;
    }
    if (sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
        portal_down(sm);
        return;
    }
    /* Reachable over something else, so the access point would add nothing but
     * an open network advertised for as long as the device is powered -- which
     * on an Ethernet-only device is for ever, because this machine never
     * reaches CONNECTED and nothing else took the portal down (espOS #158).
     *
     * Ahead of the portal_active check below on purpose: Ethernet usually
     * comes up AFTER the portal, so this has to be able to take a running one
     * down, not merely decline to raise it. */
    if (sm->other_net_up && !sm->cfg.portal_online) {
        portal_down(sm);
        return;
    }
    if (sm->st.portal_active) {
        return;
    }
    if (sm->cfg.net_count == 0 || !sm->cfg.sta_enabled) {
        portal_up(sm); /* nothing to try: help immediately */
        return;
    }
    uint32_t due = sm->st.disconnected_since_ms + sm->cfg.portal_after_ms;
    if ((int32_t)(now(sm) - due) >= 0) {
        /* Not while an association is in flight. Raising the portal switches the
         * radio to AP+STA, and a switch landing INSIDE an attempt is measurably
         * worse than one between attempts: on an ESP32-S3 against an AP that
         * already misses the occasional handshake, a portal settled beforehand
         * connected first try three times out of three, while the deadline
         * elapsing mid-attempt preceded a ~9 minute stall, every failed attempt
         * logging an APSTA profile renegotiation the portal-closed run never did
         * (espOS #144, measured downstream).
         *
         * Deferring costs at most the remainder of this attempt, because the
         * deadline is folded into the timer that is already armed for it (see
         * arm()): the attempt resolves, portal_policy() runs again from BACKOFF
         * or from the disconnected path, and `due` is long past, so the portal
         * comes up then. The deferral is bounded by the attempt rather than by
         * success, so a device that never connects still gets its portal.
         *
         * OBTAINING_IP is deliberately NOT deferred: the association is done, so
         * a mode switch cannot disturb it, and a device stuck waiting on DHCP is
         * exactly one an operator needs the portal to reach. */
        if (sm->connect_in_flight) {
            /* Clear the deadline rather than keep it pending. Leaving a PAST-DUE
             * deadline set makes arm() fold it to a 1 ms timer, which fires,
             * defers here again, re-arms at 1 ms -- a busy loop for the rest of
             * the attempt. Nothing is lost by clearing it: every path out of
             * CONNECTING (association, failure, timeout) calls portal_policy()
             * again, and `due` is computed from disconnected_since_ms, which does
             * not move while we are still disconnected -- so it is already past
             * and the portal comes up immediately then. */
            sm->portal_due_ms = 0;
            return;
        }
        portal_up(sm);
    } else {
        sm->portal_due_ms = due ? due : 1;
    }
}

/* The single timer serves the state timeout; the portal deadline is checked
 * whenever the timer fires and also folded into the arm time. */
static void arm(espos_wifi_sm_t *sm, uint32_t ms, bool is_dhcp)
{
    sm->timer_is_dhcp = is_dhcp;
    sm->timer_is_portal = false;
    sm->state_due_ms = now(sm) + ms;
    if (sm->state_due_ms == 0) {
        sm->state_due_ms = 1;
    }
    if (sm->portal_due_ms) {
        uint32_t t = now(sm);
        uint32_t until_portal = (int32_t)(sm->portal_due_ms - t) > 0 ? sm->portal_due_ms - t : 1;
        if (until_portal < ms) {
            ms = until_portal;
            sm->timer_is_portal = true; /* fires for the portal first; state timeout re-armed after */
        }
    }
    sm->timer_due_ms = now(sm) + ms;
    if (sm->timer_due_ms == 0) {
        sm->timer_due_ms = 1;
    }
    sm->port->arm_timer(sm->port_ctx, ms);
}

/* Re-arm the pending state timeout for whatever time it has left. */
/* A policy run may have scheduled the portal for later (portal_due_ms) rather
 * than raised it now, and nothing acts on that deadline unless the timer is
 * armed for it: arm() folds portal_due_ms in when it is CALLED, not
 * retroactively. Every handler that changes the policy outside the normal
 * transitions therefore ends with this, or a portal owed in 30 s arrives
 * whenever the next unrelated timeout happens to fire -- or not at all. */
static void rearm_after_policy(espos_wifi_sm_t *sm);

static void rearm_state(espos_wifi_sm_t *sm)
{
    if (!sm->state_due_ms) {
        return;
    }
    int32_t left = (int32_t)(sm->state_due_ms - now(sm));
    bool is_dhcp = sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP;
    uint32_t due = sm->state_due_ms;
    arm(sm, left > 0 ? (uint32_t)left : 1, is_dhcp);
    sm->state_due_ms = due; /* arm() recomputes it; keep the original deadline */
}

static void arm_portal_only(espos_wifi_sm_t *sm)
{
    sm->state_due_ms = 0;
    if (sm->portal_due_ms) {
        uint32_t t = now(sm);
        uint32_t ms = (int32_t)(sm->portal_due_ms - t) > 0 ? sm->portal_due_ms - t : 1;
        sm->timer_is_dhcp = false;
        sm->timer_is_portal = true;
        sm->timer_due_ms = t + ms ? t + ms : 1;
        sm->port->arm_timer(sm->port_ctx, ms);
    } else {
        sm->timer_due_ms = 0;
        sm->port->cancel_timer(sm->port_ctx);
    }
}

static void rearm_after_policy(espos_wifi_sm_t *sm)
{
    if (sm->state_due_ms) {
        rearm_state(sm); /* keeps a connect/DHCP/backoff deadline, folds the portal in */
    } else {
        arm_portal_only(sm);
    }
}

/* ------------------------------------------------------------ transitions */

static void start_attempt(espos_wifi_sm_t *sm)
{
    if (sm->cfg.net_count == 0) {
        set_state(sm, ESPOS_WIFI_ST_UNCONFIGURED);
        sm->st.net_index = -1;
        /* Nothing will be attempted, so nothing is in flight. Left set, the
         * flag would defer the portal for ever: every path that clears it is a
         * path out of an attempt, and there is no attempt to come out of. */
        sm->connect_in_flight = false;
        portal_policy(sm);
        arm_portal_only(sm);
        notify(sm);
        return;
    }
    if (sm->st.net_index < 0 || (size_t)sm->st.net_index >= sm->cfg.net_count) {
        sm->st.net_index = 0;
    }
    sm->st.attempt++;
    set_state(sm, ESPOS_WIFI_ST_CONNECTING);
    memset(&sm->st.link, 0, sizeof(sm->st.link));
    memset(&sm->st.ip, 0, sizeof(sm->st.ip));
    esp_err_t err = sm->port->connect(sm->port_ctx, &sm->cfg.nets[sm->st.net_index]);
    if (err != ESP_OK) {
        /* driver refused: treat like an immediate failure */
        sm->st.reason = ESPOS_WIFI_REASON_CONNECT_TIMEOUT;
        espos_wifi_sm_event(sm, ESPOS_WIFI_EV_STA_DISCONNECTED, &(int) { ESPOS_WIFI_REASON_CONNECT_TIMEOUT });
        return;
    }
    sm->connect_in_flight = true;
    portal_policy(sm);
    arm(sm, sm->cfg.connect_timeout_ms, false);
    notify(sm);
}

/* An attempt failed: next network, or back off after a full round. A drop
 * from CONNECTED retries the same network once first (AP reboot, roaming)
 * before moving down the priority list. */
static void attempt_failed(espos_wifi_sm_t *sm, int reason)
{
    sm->st.reason = reason;
    sm->connect_in_flight = false;
    if (sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
        sm->st.disconnect_count++;
        sm->st.disconnected_since_ms = now(sm);
        start_attempt(sm);
        return;
    }
    int next = sm->st.net_index + 1;
    if ((size_t)next < sm->cfg.net_count) {
        sm->st.net_index = next;
        /* Between networks is the only gap in a round, and the portal deadline has
         * to be honoured in it -- otherwise a device with several configured
         * networks defers through EVERY attempt of the round, making the deferral
         * bounded by the round rather than by one attempt.
         *
         * The flag, not the state: attempt_failed() does not change state, so it is
         * still CONNECTING here even though no association is in flight. Inferring
         * "in flight" from the state was wrong for exactly this gap. */
        sm->connect_in_flight = false;
        portal_policy(sm);
        start_attempt(sm);
        return;
    }
    /* full round done */
    sm->st.net_index = 0;
    uint32_t delay = espos_wifi_backoff_ms(sm->st.round, sm->cfg.backoff_max_ms, sm->port->random(sm->port_ctx));
    if (sm->st.round < 30) {
        sm->st.round++;
    }
    set_state(sm, ESPOS_WIFI_ST_BACKOFF);
    sm->st.backoff_until_ms = now(sm) + delay;
    portal_policy(sm);
    arm(sm, delay, false);
    notify(sm);
}

static void go_idle(espos_wifi_sm_t *sm, espos_wifi_state_t s, int reason)
{
    if (sm->st.state == ESPOS_WIFI_ST_CONNECTED || sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP ||
        sm->st.state == ESPOS_WIFI_ST_CONNECTING) {
        sm->port->disconnect(sm->port_ctx);
    }
    if (sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
        sm->st.disconnect_count++;
    }
    sm->st.reason = reason;
    sm->st.net_index = -1;
    sm->st.disconnected_since_ms = now(sm);
    memset(&sm->st.link, 0, sizeof(sm->st.link));
    memset(&sm->st.ip, 0, sizeof(sm->st.ip));
    /* The attempt was abandoned, not replaced: disconnect() above ended it and
     * no result will arrive for it. Cleared here rather than in the callers
     * because this is the one place that leaves the attempt cycle. */
    sm->connect_in_flight = false;
    set_state(sm, s);
    portal_policy(sm);
    arm_portal_only(sm);
    notify(sm);
}

/* ------------------------------------------------------------------ init */

void espos_wifi_sm_init(espos_wifi_sm_t *sm, const espos_wifi_port_t *port, void *port_ctx,
                        const espos_wifi_cfg_t *cfg)
{
    memset(sm, 0, sizeof(*sm));
    sm->port = port;
    sm->port_ctx = port_ctx;
    sm->cfg = *cfg;
    sm->st.state = ESPOS_WIFI_ST_DISABLED;
    sm->st.net_index = -1;
    sm->st.sta_enabled = cfg->sta_enabled;
}

const espos_wifi_sm_status_t *espos_wifi_sm_status(const espos_wifi_sm_t *sm)
{
    return &sm->st;
}

uint32_t espos_wifi_sm_backoff_remaining_ms(const espos_wifi_sm_t *sm)
{
    if (sm->st.state != ESPOS_WIFI_ST_BACKOFF) {
        return 0;
    }
    int32_t d = (int32_t)(sm->st.backoff_until_ms - sm->port->now_ms(sm->port_ctx));
    return d > 0 ? (uint32_t)d : 0;
}

static bool nets_equal(const espos_wifi_cfg_t *a, const espos_wifi_cfg_t *b)
{
    if (a->net_count != b->net_count) {
        return false;
    }
    for (size_t i = 0; i < a->net_count; i++) {
        if (strcmp(a->nets[i].ssid, b->nets[i].ssid) != 0 || strcmp(a->nets[i].psk, b->nets[i].psk) != 0 ||
            a->nets[i].has_bssid != b->nets[i].has_bssid ||
            (a->nets[i].has_bssid && memcmp(a->nets[i].bssid, b->nets[i].bssid, 6) != 0)) {
            return false;
        }
    }
    return true;
}

/* Does the currently used network still exist unchanged in the new config? */
static bool current_net_still_valid(const espos_wifi_sm_t *sm, const espos_wifi_cfg_t *ncfg, int *new_index)
{
    if (sm->st.net_index < 0 || (size_t)sm->st.net_index >= sm->cfg.net_count) {
        return false;
    }
    const espos_wifi_net_t *cur = &sm->cfg.nets[sm->st.net_index];
    for (size_t i = 0; i < ncfg->net_count; i++) {
        const espos_wifi_net_t *n = &ncfg->nets[i];
        if (strcmp(n->ssid, cur->ssid) == 0 && strcmp(n->psk, cur->psk) == 0 &&
            n->has_bssid == cur->has_bssid &&
            (!n->has_bssid || memcmp(n->bssid, cur->bssid, 6) == 0)) {
            *new_index = (int)i;
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------- events */

void espos_wifi_sm_event(espos_wifi_sm_t *sm, espos_wifi_event_t ev, const void *arg)
{
    switch (ev) {
    case ESPOS_WIFI_EV_START:
        if (sm->started) {
            return;
        }
        sm->started = true;
        sm->st.disconnected_since_ms = now(sm);
        sm->st.round = 0;
        sm->st.attempt = 0;
        if (!sm->cfg.sta_enabled) {
            go_idle(sm, ESPOS_WIFI_ST_DISABLED, ESPOS_WIFI_REASON_DISABLED);
        } else {
            sm->st.net_index = 0;
            start_attempt(sm);
        }
        return;

    case ESPOS_WIFI_EV_STOP:
        if (!sm->started) {
            return;
        }
        sm->port->cancel_timer(sm->port_ctx);
        sm->timer_due_ms = 0;
        sm->state_due_ms = 0;
        portal_down(sm);
        if (sm->st.state == ESPOS_WIFI_ST_CONNECTED || sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP ||
            sm->st.state == ESPOS_WIFI_ST_CONNECTING) {
            sm->port->disconnect(sm->port_ctx);
        }
        sm->started = false;
        set_state(sm, ESPOS_WIFI_ST_DISABLED);
        sm->st.net_index = -1;
        notify(sm);
        return;

    case ESPOS_WIFI_EV_CONFIG: {
        const espos_wifi_cfg_t *ncfg = arg;
        if (memcmp(ncfg, &sm->cfg, sizeof(*ncfg)) == 0) {
            return; /* nothing changed */
        }
        if (nets_equal(ncfg, &sm->cfg) && ncfg->sta_enabled == sm->cfg.sta_enabled) {
            /* timing / portal knobs only: take them, re-evaluate the portal, keep going */
            sm->cfg = *ncfg;
            if (sm->started) {
                portal_policy(sm);
                if (sm->st.state == ESPOS_WIFI_ST_UNCONFIGURED || sm->st.state == ESPOS_WIFI_ST_DISABLED) {
                    arm_portal_only(sm);
                }
                notify(sm);
            }
            return;
        }
        int keep_index = -1;
        bool keep = sm->started && ncfg->sta_enabled &&
                    (sm->st.state == ESPOS_WIFI_ST_CONNECTED || sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP) &&
                    current_net_still_valid(sm, ncfg, &keep_index);
        bool was_enabled = sm->cfg.sta_enabled;
        sm->cfg = *ncfg;
        sm->st.sta_enabled = ncfg->sta_enabled;
        if (!sm->started) {
            return;
        }
        if (keep) {
            /* connection survives; only bookkeeping and portal policy change */
            sm->st.net_index = keep_index;
            portal_policy(sm);
            notify(sm);
            return;
        }
        if (!ncfg->sta_enabled) {
            go_idle(sm, ESPOS_WIFI_ST_DISABLED, ESPOS_WIFI_REASON_DISABLED);
            return;
        }
        /* fresh start with the new list (also covers "was disabled, now enabled") */
        if (sm->st.state == ESPOS_WIFI_ST_CONNECTED || sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP ||
            sm->st.state == ESPOS_WIFI_ST_CONNECTING) {
            sm->port->disconnect(sm->port_ctx);
            if (sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
                sm->st.disconnect_count++;
                sm->st.disconnected_since_ms = now(sm);
            }
        }
        (void)was_enabled;
        sm->st.reason = ESPOS_WIFI_REASON_CONFIG_CHANGE;
        sm->st.round = 0;
        sm->st.attempt = 0;
        sm->st.net_index = 0;
        start_attempt(sm);
        return;
    }

    case ESPOS_WIFI_EV_STA_CONNECTED:
        if (sm->st.state != ESPOS_WIFI_ST_CONNECTING) {
            return; /* stale */
        }
        if (arg) {
            sm->st.link = *(const espos_wifi_link_t *)arg;
        }
        sm->connect_in_flight = false;
        set_state(sm, ESPOS_WIFI_ST_OBTAINING_IP);
        /* Re-evaluate before arming: the association we deferred the portal for has
         * just finished, so a deadline that elapsed during it is due NOW. Without
         * this the portal stays deferred through the whole DHCP wait -- which is
         * the opposite of what OBTAINING_IP is supposed to mean here, and leaves a
         * device stuck on DHCP unreachable for dhcp_timeout_ms. */
        portal_policy(sm);
        arm(sm, sm->cfg.dhcp_timeout_ms, true);
        notify(sm);
        return;

    case ESPOS_WIFI_EV_STA_DISCONNECTED: {
        int reason = arg ? *(const int *)arg : 0;
        if (sm->st.state == ESPOS_WIFI_ST_CONNECTING || sm->st.state == ESPOS_WIFI_ST_OBTAINING_IP ||
            sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
            attempt_failed(sm, reason);
        }
        return;
    }

    case ESPOS_WIFI_EV_GOT_IP:
        /* Only after association (first lease) or while connected (renewal).
         * A lease seen in CONNECTING is stale from the previous association
         * (the driver always reports STA_CONNECTED before GOT_IP). */
        if (sm->st.state != ESPOS_WIFI_ST_OBTAINING_IP && sm->st.state != ESPOS_WIFI_ST_CONNECTED) {
            return;
        }
        if (arg) {
            sm->st.ip = *(const espos_wifi_ip_t *)arg;
        }
        if (sm->st.state != ESPOS_WIFI_ST_CONNECTED) {
            sm->st.connect_count++;
            sm->st.connected_since_ms = now(sm);
            sm->st.round = 0;
            sm->st.attempt = 0;
            sm->st.reason = ESPOS_WIFI_REASON_NONE;
        }
        set_state(sm, ESPOS_WIFI_ST_CONNECTED);
        sm->port->cancel_timer(sm->port_ctx);
        sm->timer_is_dhcp = false;
        sm->state_due_ms = 0;
        sm->timer_due_ms = 0;
        portal_policy(sm); /* takes the portal down */
        notify(sm);
        return;

    case ESPOS_WIFI_EV_LOST_IP:
        if (sm->st.state == ESPOS_WIFI_ST_CONNECTED) {
            /* Still associated; give DHCP another chance before declaring failure. */
            sm->st.disconnect_count++;
            sm->st.disconnected_since_ms = now(sm);
            memset(&sm->st.ip, 0, sizeof(sm->st.ip));
            sm->st.reason = ESPOS_WIFI_REASON_LOST_IP;
            set_state(sm, ESPOS_WIFI_ST_OBTAINING_IP);
            portal_policy(sm);
            arm(sm, sm->cfg.dhcp_timeout_ms, true);
            notify(sm);
        }
        return;

    case ESPOS_WIFI_EV_TIMER:
        if (!sm->started) {
            return;
        }
        /* A fire that was queued behind the lock while we re-armed (or
         * cancelled) the timer would act on the wrong state: only honour a
         * fire once its own deadline has passed. Ports round the arm time UP
         * to whole ticks so a legitimate expiry is never early. */
        if (sm->timer_due_ms == 0 || (int32_t)(now(sm) - sm->timer_due_ms) < 0) {
            return;
        }
        sm->timer_due_ms = 0;
        if (sm->timer_is_portal) {
            sm->timer_is_portal = false;
            portal_policy(sm);
            /* the state timeout keeps its original deadline */
            switch (sm->st.state) {
            case ESPOS_WIFI_ST_BACKOFF:
                if (espos_wifi_sm_backoff_remaining_ms(sm) == 0) {
                    start_attempt(sm);
                } else {
                    rearm_state(sm);
                }
                break;
            case ESPOS_WIFI_ST_CONNECTING:
            case ESPOS_WIFI_ST_OBTAINING_IP:
                rearm_state(sm);
                break;
            default:
                arm_portal_only(sm);
                break;
            }
            notify(sm);
            return;
        }
        sm->state_due_ms = 0;
        switch (sm->st.state) {
        case ESPOS_WIFI_ST_BACKOFF:
            start_attempt(sm);
            break;
        case ESPOS_WIFI_ST_CONNECTING:
            sm->port->disconnect(sm->port_ctx);
            attempt_failed(sm, ESPOS_WIFI_REASON_CONNECT_TIMEOUT);
            break;
        case ESPOS_WIFI_ST_OBTAINING_IP:
            sm->port->disconnect(sm->port_ctx);
            attempt_failed(sm, ESPOS_WIFI_REASON_DHCP_TIMEOUT);
            break;
        default:
            break;
        }
        return;

    case ESPOS_WIFI_EV_PORTAL_CLIENT:
        /* The port counts on the event loop while the portal is stopped
         * elsewhere, so a late count can arrive after the stop. Nobody is on a
         * portal that is down, and the stall check must not think otherwise. */
        sm->st.portal_clients = sm->st.portal_active && arg ? *(const int *)arg : 0;
        notify(sm);
        return;

    case ESPOS_WIFI_EV_PORTAL_FAILED:
        /* the driver could not bring the AP up: forget it and retry in 10 s */
        if (sm->st.portal_active) {
            sm->st.portal_active = false;
            sm->st.portal_clients = 0;
            sm->portal_due_ms = now(sm) + 10000;
            if (sm->portal_due_ms == 0) {
                sm->portal_due_ms = 1;
            }
            if (sm->state_due_ms) {
                rearm_state(sm);
            } else {
                arm_portal_only(sm);
            }
            notify(sm);
        }
        return;

    case ESPOS_WIFI_EV_PORTAL_RECONFIG:
        /* portal SSID/password changed while it is up: bounce it */
        if (sm->st.portal_active) {
            portal_down(sm);
            portal_policy(sm);
            notify(sm);
        }
        return;

    case ESPOS_WIFI_EV_OTHER_NET: {
        const bool up = arg ? *(const bool *)arg : false;
        if (up == sm->other_net_up) {
            return; /* the bus repeats NETWORK_UP whenever the route moves */
        }
        sm->other_net_up = up;
        /* Recorded but not acted on before EV_START, which seeds this on
         * purpose: a policy run here would drive the port -- raise a portal,
         * cancel a timer -- on a machine whose driver is not started, and
         * EV_START's own first policy run applies the stored value anyway. */
        if (!sm->started) {
            return;
        }
        /* Both directions matter: Ethernet appearing takes the portal down,
         * Ethernet going away is what puts it back -- and `due` is computed
         * from disconnected_since_ms, which has not moved while we sat here
         * disconnected, so it is usually already past and the portal returns
         * at once rather than after another portal_after_s. */
        portal_policy(sm);
        rearm_after_policy(sm);
        notify(sm);
        return;
    }

    case ESPOS_WIFI_EV_PORTAL_FORCE: {
        const espos_wifi_portal_force_t want = arg ? *(const espos_wifi_portal_force_t *)arg : ESPOS_WIFI_PORTAL_AUTO;
        if (want >= ESPOS_WIFI_PORTAL_FORCE_MAX) {
            return;
        }
        if (want == sm->st.portal_force) {
            return;
        }
        sm->st.portal_force = want;
        if (!sm->started) {
            return; /* as above: stored now, applied by EV_START */
        }
        portal_policy(sm);
        rearm_after_policy(sm);
        notify(sm);
        return;
    }

    case ESPOS_WIFI_EV_MAX:
        return; /* not an event; named so the switch stays exhaustive */
    }
}
