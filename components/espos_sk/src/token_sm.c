/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * SignalK access-token state machine (see espos_sk_token_sm.h). Runs on one
 * task; the port performs HTTP asynchronously and reports back with events.
 */
#include <string.h>
#include <stdio.h>
#include "espos_sk_token_sm.h"

#define POLL_MIN_MS 5000u
#define POLL_MAX_MS 60000u
#define ERR_MIN_MS  10000u
#define ERR_MAX_MS  300000u
/* A duplicate pending request backs off from a minute to ten. Not the error
 * ladder's own numbers: this is not a fault to retry out of, it is a wait for a
 * person, and ten minutes is short enough that a device is picked up soon after
 * someone approves it. */
#define DUP_MIN_MS    60000u
#define DUP_MAX_MS    600000u
#define OPEN_CHECK_MS 60000u
/* Flat, not exponential. A certificate problem is fixed from outside — the
 * server renews, or an operator presses "trust the new certificate" — and the
 * device should pick that up within a minute rather than an hour, which is
 * where an exponential backoff lands after an afternoon of trying. */
#define CERT_RETRY_MS 60000u
/* How long to wait before asking a second time after a plaintext 401. Short:
 * a real revocation should still be noticed within seconds, and the point of
 * the pause is only to let a transient interceptor get out of the way. */
#define PLAIN_RECHECK_MS 5000u

static void notify(espos_sk_tok_sm_t *sm)
{
    if (sm->port->status_changed) {
        sm->port->status_changed(sm->ctx);
    }
}

static uint32_t now(espos_sk_tok_sm_t *sm)
{
    return sm->port->now_ms(sm->ctx);
}

static void set_error(espos_sk_tok_sm_t *sm, const char *msg)
{
    snprintf(sm->st.last_error, sizeof(sm->st.last_error), "%s", msg ? msg : "");
}

/* Does this 400 mean "a request from this device is already pending"?
 *
 * Matched on the server's message because that is the only signal it gives:
 * every 400 on the access-request endpoint carries the same status and no code.
 * The substring is the stable part of signalk-server's sentence ("A device with
 * clientId '<uuid>' has already requested access"); the uuid in the middle is
 * why this is a search and not a comparison. A message we do not recognise is
 * treated as some other 400, which is the safe way round: the generic path
 * reports the server's text and backs off as an error, rather than advising
 * somebody to approve a request that does not exist. */
static bool dup_request(const char *msg)
{
    return msg && strstr(msg, "already requested access") != NULL;
}

static void arm(espos_sk_tok_sm_t *sm, uint32_t ms)
{
    sm->st.next_action_ms = now(sm) + ms;
    sm->timer_due_ms = sm->st.next_action_ms ? sm->st.next_action_ms : 1;
    sm->port->arm_timer(sm->ctx, ms);
}

static void cancel(espos_sk_tok_sm_t *sm)
{
    sm->timer_due_ms = 0;
    sm->st.next_action_ms = 0;
    sm->port->cancel_timer(sm->ctx);
}

static void save(espos_sk_tok_sm_t *sm)
{
    sm->port->store_save(sm->ctx, &sm->store);
    sm->st.has_token = sm->store.token[0] != '\0';
    snprintf(sm->st.pending_href, sizeof(sm->st.pending_href), "%s", sm->store.pending_href);
}

static void clear_pending(espos_sk_tok_sm_t *sm)
{
    sm->store.pending_href[0] = '\0';
    sm->store.pending_host[0] = '\0';
    sm->store.pending_self[0] = '\0';
    sm->store.pending_port = 0;
}

static void clear_token(espos_sk_tok_sm_t *sm)
{
    sm->store.token[0] = '\0';
    sm->store.token_self[0] = '\0';
    sm->store.token_host[0] = '\0';
    sm->store.token_port = 0;
}

/* Record where the token works. Returns true when that changed. */
static bool set_token_addr(espos_sk_tok_sm_t *sm, const char *host, uint16_t port)
{
    if (strcmp(sm->store.token_host, host) == 0 && sm->store.token_port == port) {
        return false;
    }
    snprintf(sm->store.token_host, sizeof(sm->store.token_host), "%s", host);
    sm->store.token_port = port;
    return true;
}

/* ---------------------------------------------------------- actions */

static void do_request(espos_sk_tok_sm_t *sm)
{
    sm->st.state = ESPOS_SK_TOK_IDLE;
    sm->st.busy = true;
    sm->st.request_count++;
    cancel(sm);
    sm->port->http_request(sm->ctx, &sm->st.server, &sm->cfg);
    notify(sm);
}

static void do_poll(espos_sk_tok_sm_t *sm)
{
    sm->st.state = ESPOS_SK_TOK_REQUESTED;
    sm->st.busy = true;
    cancel(sm);
    sm->port->http_poll(sm->ctx, &sm->st.server, sm->store.pending_href);
    notify(sm);
}

static void do_verify(espos_sk_tok_sm_t *sm, bool as_check)
{
    if (!as_check) {
        sm->st.state = ESPOS_SK_TOK_VERIFYING;
    }
    sm->st.busy = true;
    cancel(sm);
    sm->port->http_verify(sm->ctx, &sm->st.server, sm->store.token);
    notify(sm);
}

static void enter_error(espos_sk_tok_sm_t *sm, const char *msg)
{
    sm->st.state = ESPOS_SK_TOK_ERROR;
    set_error(sm, msg);
    uint32_t d = sm->error_backoff_ms;
    /* ±20 % jitter */
    uint32_t j = sm->port->random(sm->ctx) % (d / 5 + 1);
    arm(sm, d - d / 10 + j);
    if (sm->error_backoff_ms < ERR_MAX_MS) {
        sm->error_backoff_ms *= 2;
        if (sm->error_backoff_ms > ERR_MAX_MS) {
            sm->error_backoff_ms = ERR_MAX_MS;
        }
    }
    notify(sm);
}

/* The transport refused the server. The token is kept on purpose: it is still
 * a perfectly good credential for that server, and dropping it would mean a
 * fresh approval in the admin UI every time a certificate is renewed. */
static void enter_cert_error(espos_sk_tok_sm_t *sm, const char *reason)
{
    sm->st.state = ESPOS_SK_TOK_CERT_ERROR;
    set_error(sm, reason && reason[0] ? reason : "the server's certificate is not the one this device trusts");
    sm->st.cert_error_count++;
    arm(sm, CERT_RETRY_MS);
    notify(sm);
}

/* A 401/403 while our token was on the request. Over TLS nobody but the
 * server can have answered, so one is conclusive; over plaintext anything on
 * the path can produce one — a captive portal, a proxy, a router's login page
 * — and clearing the token costs a trip to the server's admin UI. So the
 * plaintext case wants a second opinion: keep the token, come back in 5 s with
 * a verify leg, and only clear if that is refused too.
 *
 * Returns true when the token was cleared (the caller then re-requests). */
static bool handle_unauthorized(espos_sk_tok_sm_t *sm)
{
    sm->st.unauthorized_count++;
    if (!sm->st.server.tls) {
        sm->plain_unauth_streak++;
        if (sm->plain_unauth_streak < 2) {
            set_error(sm, "unauthorized over plaintext; checking again before dropping the token");
            arm(sm, PLAIN_RECHECK_MS);
            notify(sm);
            return false;
        }
    }
    if (sm->st.server.by_self) {
        /* Likely a twin with our self URN: its "no" is about its own users.
         * Clearing here is how a server restart used to cost the pairing --
         * the twin answered while ours was down, and ours came back to a
         * device with no token. Wait for ours; pinning this server by hand
         * (a real move) makes its answer count again. */
        sm->plain_unauth_streak = 0;
        char m[ESPOS_SK_MSG_MAX];
        snprintf(m, sizeof(m), "token refused here; waiting for %.40s or a manual pin", sm->store.token_host);
        enter_error(sm, m);
        return false;
    }
    sm->plain_unauth_streak = 0;
    clear_token(sm);
    save(sm);
    set_error(sm, "token rejected by the server; requesting access again");
    return true;
}

/* Decide what to do for the current server given what we have stored. */
static void evaluate(espos_sk_tok_sm_t *sm)
{
    if (!sm->started) {
        return;
    }
    if (!sm->st.has_server) {
        sm->st.state = ESPOS_SK_TOK_NO_SERVER;
        cancel(sm);
        notify(sm);
        return;
    }
    const espos_sk_server_t *srv = &sm->st.server;
    /* Resume a pending request if it belongs to this server. */
    if (sm->store.pending_href[0]) {
        bool same = (srv->self[0] && sm->store.pending_self[0] && strcmp(srv->self, sm->store.pending_self) == 0) ||
                    (strcmp(srv->host, sm->store.pending_host) == 0 && srv->port == sm->store.pending_port);
        if (same) {
            sm->st.poll_interval_ms = POLL_MIN_MS;
            if (!sm->st.requested_since_ms) {
                sm->st.requested_since_ms = now(sm); /* resumed after a reboot: age unknown */
            }
            do_poll(sm);
            return;
        }
        clear_pending(sm); /* was for another server */
        save(sm);
    }
    /* Try a stored token if it is (or may be) for this server. */
    if (sm->store.token[0]) {
        bool applicable = sm->store.token_self[0] == '\0' || srv->self[0] == '\0' ||
                          strcmp(sm->store.token_self, srv->self) == 0;
        if (applicable) {
            /* Over TLS, skip straight to APPROVED and let the WebSocket
             * upgrade be the check. It carries the same token and answers
             * 401 just as plainly, so the verify leg would only buy a second
             * TLS handshake on every reconnect — the expensive part, in the
             * memory pool that is scarcest. Over plaintext the leg is nearly
             * free and still worth having.
             *
             * A token that has in fact been revoked is caught at the upgrade
             * and comes back as EV_UNAUTHORIZED, so nothing is lost but a
             * handshake. */
            if (srv->tls) {
                sm->st.state = ESPOS_SK_TOK_APPROVED;
                if (!sm->st.approved_since_ms) {
                    sm->st.approved_since_ms = now(sm);
                }
                sm->error_backoff_ms = ERR_MIN_MS;
                sm->dup_backoff_ms = DUP_MIN_MS;
                set_error(sm, "");
                arm(sm, sm->cfg.check_interval_ms);
                notify(sm);
                return;
            }
            do_verify(sm, false);
            return;
        }
        /* token belongs to a different server (self mismatch): keep it stored
         * (that server may come back) but request access to this one */
    }
    do_request(sm);
}

/* ------------------------------------------------------------ public */

const char *espos_sk_tok_state_str(espos_sk_tok_state_t s)
{
    switch (s) {
    case ESPOS_SK_TOK_NO_SERVER: return "no_server";
    case ESPOS_SK_TOK_IDLE: return "requesting";
    case ESPOS_SK_TOK_REQUESTED: return "pending";
    case ESPOS_SK_TOK_VERIFYING: return "verifying";
    case ESPOS_SK_TOK_APPROVED: return "approved";
    case ESPOS_SK_TOK_DENIED: return "denied";
    case ESPOS_SK_TOK_OPEN: return "open";
    case ESPOS_SK_TOK_ERROR: return "error";
    case ESPOS_SK_TOK_CERT_ERROR: return "cert_error";
    }
    return "unknown";
}

void espos_sk_tok_init(espos_sk_tok_sm_t *sm, const espos_sk_tok_port_t *port, void *ctx,
                       const espos_sk_tok_cfg_t *cfg, const espos_sk_tok_store_t *store)
{
    memset(sm, 0, sizeof(*sm));
    sm->port = port;
    sm->ctx = ctx;
    sm->cfg = *cfg;
    if (store) {
        sm->store = *store;
    }
    sm->st.state = ESPOS_SK_TOK_NO_SERVER;
    sm->st.has_token = sm->store.token[0] != '\0';
    snprintf(sm->st.pending_href, sizeof(sm->st.pending_href), "%s", sm->store.pending_href);
    sm->error_backoff_ms = ERR_MIN_MS;
    sm->dup_backoff_ms = DUP_MIN_MS;
    sm->st.poll_interval_ms = POLL_MIN_MS;
}

const espos_sk_tok_status_t *espos_sk_tok_status(const espos_sk_tok_sm_t *sm)
{
    return &sm->st;
}

const char *espos_sk_tok_token(const espos_sk_tok_sm_t *sm)
{
    return (sm->st.state == ESPOS_SK_TOK_APPROVED) ? sm->store.token : "";
}

void espos_sk_tok_event(espos_sk_tok_sm_t *sm, espos_sk_tok_event_t ev, const void *arg)
{
    switch (ev) {
    case ESPOS_SK_EV_START:
        if (sm->started) {
            return;
        }
        sm->started = true;
        evaluate(sm);
        return;

    case ESPOS_SK_EV_STOP:
        sm->started = false;
        sm->st.busy = false;
        sm->reeval = false;
        cancel(sm);
        sm->st.state = ESPOS_SK_TOK_NO_SERVER;
        notify(sm);
        return;

    case ESPOS_SK_EV_CONFIG:
        if (arg) {
            bool interval_changed = sm->cfg.check_interval_ms != ((const espos_sk_tok_cfg_t *)arg)->check_interval_ms;
            sm->cfg = *(const espos_sk_tok_cfg_t *)arg;
            if (interval_changed && sm->st.state == ESPOS_SK_TOK_APPROVED && !sm->st.busy) {
                arm(sm, sm->cfg.check_interval_ms); /* apply the new check cadence now */
            }
        }
        return;

    case ESPOS_SK_EV_SERVER: {
        const espos_sk_server_t *srv = arg;
        /* The scheme is part of the address, not a detail of it: the same
         * host and port over https is a different endpoint, with a different
         * certificate to judge and a transport that has to be rebuilt. Left
         * out of this comparison, a switch to or from TLS was accepted by the
         * configuration and by select_server() and then silently dropped
         * here, so the device kept talking the old scheme for ever. */
        bool same_addr = srv && sm->st.has_server && strcmp(srv->host, sm->st.server.host) == 0 &&
                         srv->port == sm->st.server.port && srv->tls == sm->st.server.tls;
        bool self_compatible = srv && (srv->self[0] == '\0' || sm->st.server.self[0] == '\0' ||
                                       strcmp(srv->self, sm->st.server.self) == 0);
        if (same_addr && self_compatible) {
            /* Whether it stands in for our server can change without the
             * address changing (a request made here makes it the anchor). */
            if (sm->st.server.by_self != srv->by_self) {
                sm->st.server.by_self = srv->by_self;
                notify(sm);
            }
            /* same server; a self URN may have become known via mDNS */
            if (srv->self[0] && !sm->st.server.self[0]) {
                snprintf(sm->st.server.self, sizeof(sm->st.server.self), "%s", srv->self);
                if (sm->store.token[0] && !sm->store.token_self[0]) {
                    snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", srv->self);
                    save(sm);
                }
                notify(sm);
            }
            return;
        }
        if (srv) {
            sm->st.server = *srv;
            sm->st.has_server = true;
        } else {
            memset(&sm->st.server, 0, sizeof(sm->st.server));
            sm->st.has_server = false;
        }
        sm->error_backoff_ms = ERR_MIN_MS;
        sm->dup_backoff_ms = DUP_MIN_MS;
        set_error(sm, "");
        if (sm->st.busy) {
            /* an action for the old server is in flight: its result must not
             * be attributed to the new one — re-evaluate when it lands */
            sm->reeval = true;
            notify(sm);
            return;
        }
        evaluate(sm);
        return;
    }

    case ESPOS_SK_EV_REQUEST_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        if (sm->st.state != ESPOS_SK_TOK_IDLE || !sm->st.busy) {
            return; /* stale */
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        if (r->cert_error) {
            enter_cert_error(sm, r->cert_reason);
            return;
        }
        if (r->http_status == 202 && r->href[0]) {
            snprintf(sm->store.pending_href, sizeof(sm->store.pending_href), "%s", r->href);
            snprintf(sm->store.pending_host, sizeof(sm->store.pending_host), "%s", sm->st.server.host);
            snprintf(sm->store.pending_self, sizeof(sm->store.pending_self), "%s", sm->st.server.self);
            sm->store.pending_port = sm->st.server.port;
            save(sm);
            sm->st.state = ESPOS_SK_TOK_REQUESTED;
            sm->st.requested_since_ms = now(sm);
            sm->st.poll_interval_ms = POLL_MIN_MS;
            sm->error_backoff_ms = ERR_MIN_MS;
            sm->dup_backoff_ms = DUP_MIN_MS;
            set_error(sm, "");
            arm(sm, POLL_MIN_MS);
            notify(sm);
            return;
        }
        if (r->http_status == 404) {
            /* "Server security is not enabled": nothing to request */
            sm->st.state = ESPOS_SK_TOK_OPEN;
            sm->error_backoff_ms = ERR_MIN_MS;
            sm->dup_backoff_ms = DUP_MIN_MS;
            set_error(sm, "");
            arm(sm, OPEN_CHECK_MS);
            notify(sm);
            return;
        }
        if (r->http_status == 599) {
            enter_error(sm, "host answers but is not a SignalK server");
            return;
        }
        if (r->http_status == 403) {
            sm->st.state = ESPOS_SK_TOK_DENIED;
            sm->st.deny_count++;
            set_error(sm, "device access requests are disabled on the server");
            cancel(sm);
            notify(sm);
            return;
        }
        if (r->http_status == 400 && dup_request(r->message)) {
            /* "already requested access": a request this device made and lost
             * track of, sitting unanswered on the server. Once someone decides
             * it, a new one goes through.
             *
             * Only this 400 gets the treatment below. signalk-server answers 400
             * to three other things on this endpoint -- a body that fails its
             * validation, a permissions value it does not know, and a duplicate
             * USER (unreachable from here, we always send a clientId) -- and all
             * of those are bugs on this side, not a person who has not looked
             * yet. Telling an operator to go approve something would send them
             * to an empty Access Requests list, and stretching the retry to ten
             * minutes would hide the bug rather than surface it, so those fall
             * through to the generic error path with the server\'s own words.
             *
             * This backs off like every other error rather than retrying on a
             * fixed minute. A 400 here is not transient -- it says a human has
             * not looked yet -- so asking again at the same rate forever
             * achieves nothing and is not free: measured on an ESP32-C5, 31
             * such requests over ~34 minutes spiked the heap on every cycle
             * while the deltas that could not be sent accumulated, and the
             * combination fragmented internal RAM until the health watchdog
             * rebooted the board (espOS #128). The first retry is still prompt,
             * because the usual case IS someone approving it within a minute or
             * two; it is the twentieth that has no business being prompt.
             *
             * The cap matters as much as the growth: DUP_MAX_MS bounds how long
             * a device can sit unnoticed after approval, so nobody has to
             * power-cycle it to be seen. */
            sm->st.state = ESPOS_SK_TOK_ERROR;
            /* What the reader must DO, not what the server said. Its own
             * wording ("A device with clientId '<uuid>' has already requested
             * access") is accurate but describes the server's state, names a
             * uuid nobody can act on, and does not fit: ESPOS_SK_MSG_MAX is 96
             * bytes and the clientId alone eats half of it, so appending advice
             * would truncate one or the other. Now that the retry stretches to
             * ten minutes it matters more than it did -- a status line reading
             * like a stuck device rather than one waiting for a person is the
             * difference between somebody approving it and somebody
             * power-cycling it. The raw server text is still in
             * last_error/last_http_status for anyone debugging. */
            set_error(sm, "already asked; approve it in Security -> Access Requests");
            uint32_t d = sm->dup_backoff_ms;
            uint32_t j = sm->port->random(sm->ctx) % (d / 5 + 1);
            arm(sm, d - d / 10 + j);
            if (sm->dup_backoff_ms < DUP_MAX_MS) {
                sm->dup_backoff_ms *= 2;
                if (sm->dup_backoff_ms > DUP_MAX_MS) {
                    sm->dup_backoff_ms = DUP_MAX_MS;
                }
            }
            notify(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else if (r->message[0]) {
            /* Prefer what the server said over restating its status code. On a
             * 400 that is not the duplicate case above, its message is the only
             * thing that says WHICH request it rejected and why ("invalid
             * permissions", a body that failed validation), and that is a bug on
             * this side that someone has to read to fix.
             *
             * Passed straight through rather than prefixed with the status: a
             * server message can be as long as the field itself, and adding
             * "HTTP %d: " would push the tail of the sentence out of a
             * 96-byte buffer (-Werror=format-truncation says so at compile
             * time). The code is in last_http_status either way. */
            enter_error(sm, r->message);
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "request failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_POLL_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        if (sm->st.state != ESPOS_SK_TOK_REQUESTED || !sm->st.busy) {
            return;
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        if (r->cert_error) {
            enter_cert_error(sm, r->cert_reason);
            return;
        }
        if ((r->http_status == 200 || r->http_status == 202) && r->state[0] == '\0') {
            enter_error(sm, "unexpected reply while polling (not a SignalK server?)");
            return;
        }
        if (r->http_status == 200 || r->http_status == 202) {
            sm->error_backoff_ms = ERR_MIN_MS; /* the server answers again */
            sm->dup_backoff_ms = DUP_MIN_MS;
            if (strcmp(r->state, "COMPLETED") == 0) {
                if (strcmp(r->permission, "APPROVED") == 0 && r->token[0]) {
                    snprintf(sm->store.token, sizeof(sm->store.token), "%s", r->token);
                    snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", sm->st.server.self);
                    set_token_addr(sm, sm->st.server.host, sm->st.server.port);
                    clear_pending(sm);
                    save(sm);
                    sm->st.approve_count++;
                    set_error(sm, "");
                    do_verify(sm, false);
                    return;
                }
                /* DENIED, or COMPLETED with an error statusCode */
                clear_pending(sm);
                save(sm);
                sm->st.state = ESPOS_SK_TOK_DENIED;
                sm->st.deny_count++;
                set_error(sm, r->message[0] ? r->message : (strcmp(r->permission, "DENIED") == 0 ? "access denied by the server admin" : "request rejected"));
                cancel(sm);
                notify(sm);
                return;
            }
            /* PENDING: keep polling with backoff */
            uint32_t next = sm->st.poll_interval_ms + sm->st.poll_interval_ms / 2;
            if (next > POLL_MAX_MS) {
                next = POLL_MAX_MS;
            }
            sm->st.poll_interval_ms = next;
            arm(sm, next);
            notify(sm);
            return;
        }
        if (r->http_status == 404 || (r->http_status == 500 && strstr(r->message, "not found"))) {
            if (sm->st.server.by_self) {
                /* A twin with our self URN never heard of the request; the
                 * server that did is only out of sight. Keep it for that one. */
                char m[ESPOS_SK_MSG_MAX];
                snprintf(m, sizeof(m), "request unknown here; waiting for %.40s or a manual pin",
                         sm->store.pending_host);
                enter_error(sm, m);
                return;
            }
            /* the server forgot the request (restart, prune): start over */
            clear_pending(sm);
            save(sm);
            set_error(sm, "server lost the request; requesting again");
            do_request(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "poll failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_VERIFY_RESULT: {
        const espos_sk_http_result_t *r = arg;
        if (sm->reeval && sm->st.busy) {
            /* stale answer for a previous server/token: drop it, start over */
            sm->reeval = false;
            sm->st.busy = false;
            evaluate(sm);
            return;
        }
        bool checking = sm->st.state == ESPOS_SK_TOK_APPROVED || sm->st.state == ESPOS_SK_TOK_OPEN;
        if ((sm->st.state != ESPOS_SK_TOK_VERIFYING && !checking) || !sm->st.busy) {
            return;
        }
        sm->st.busy = false;
        sm->st.last_http_status = r->http_status;
        sm->st.last_check_ms = now(sm);
        if (r->cert_error) {
            enter_cert_error(sm, r->cert_reason);
            return;
        }
        if (r->http_status == 200) {
            sm->plain_unauth_streak = 0; /* the server answers us: whatever the 401 was, it is over */
            if (sm->st.state == ESPOS_SK_TOK_OPEN) {
                arm(sm, OPEN_CHECK_MS); /* still open */
                notify(sm);
                return;
            }
            /* A token stored before the address was recorded learns it here.
             * One that has an address keeps it: a 200 is not proof this is
             * the issuing server -- a same-self twin with security off
             * answers 200 to anything -- and re-homing on it would anchor the
             * device to the twin for good. A server that moved is found by
             * self once its old address has gone quiet (espos_sk.c). */
            bool changed = !sm->store.token_host[0] && set_token_addr(sm, sm->st.server.host, sm->st.server.port);
            if (r->self[0]) {
                changed |= strcmp(sm->store.token_self, r->self) != 0;
                snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s", r->self);
                if (sm->st.server.self[0] == '\0') {
                    snprintf(sm->st.server.self, sizeof(sm->st.server.self), "%s", r->self);
                }
            }
            if (changed) {
                save(sm);
            }
            if (sm->st.state != ESPOS_SK_TOK_APPROVED) {
                sm->st.approved_since_ms = now(sm);
            }
            sm->st.state = ESPOS_SK_TOK_APPROVED;
            sm->error_backoff_ms = ERR_MIN_MS;
            sm->dup_backoff_ms = DUP_MIN_MS;
            set_error(sm, "");
            arm(sm, sm->cfg.check_interval_ms);
            notify(sm);
            return;
        }
        if (r->http_status == 401 || r->http_status == 403) {
            if (sm->st.state == ESPOS_SK_TOK_OPEN) {
                /* security got enabled: go get a token */
                set_error(sm, "");
                do_request(sm);
                return;
            }
            if (!handle_unauthorized(sm)) {
                /* Plaintext, first refusal: keep the token, ask again in a
                 * moment. Stay APPROVED meanwhile — the credential has not
                 * been shown to be bad, and demoting the state would take the
                 * stream down over what is very often a passing proxy. */
                return;
            }
            do_request(sm);
            return;
        }
        if (r->http_status == 0) {
            enter_error(sm, "server unreachable");
        } else {
            char m[ESPOS_SK_MSG_MAX];
            snprintf(m, sizeof(m), "check failed (HTTP %d)", r->http_status);
            enter_error(sm, m);
        }
        return;
    }

    case ESPOS_SK_EV_TIMER:
        if (!sm->started || sm->st.busy || sm->timer_due_ms == 0 ||
            (int32_t)(now(sm) - sm->timer_due_ms) < 0) {
            return;
        }
        sm->timer_due_ms = 0;
        switch (sm->st.state) {
        case ESPOS_SK_TOK_REQUESTED:
            do_poll(sm);
            break;
        case ESPOS_SK_TOK_APPROVED:
        case ESPOS_SK_TOK_VERIFYING:
            /* VERIFYING is armed only by the plaintext second-opinion pause:
             * a first 401 during the boot check or a manual paste leaves the
             * machine here with the token still stored, waiting to ask again. */
            do_verify(sm, true);
            break;
        case ESPOS_SK_TOK_OPEN:
            /* GET /self succeeds without a token when allow_readonly is on,
             * so a re-POST is the only reliable probe: 404 = still open,
             * 202 = security got enabled and we are already in the queue. */
            do_request(sm);
            break;
        case ESPOS_SK_TOK_ERROR:
        case ESPOS_SK_TOK_CERT_ERROR:
            /* Same move for both: work out what this server needs now. From
             * CERT_ERROR that re-runs the leg that failed, which is the only
             * way to find out whether the certificate has been renewed or
             * newly trusted — there is nothing to poll but the handshake. */
            evaluate(sm);
            break;
        default:
            break;
        }
        return;

    case ESPOS_SK_EV_MANUAL_TOKEN: {
        const char *tok = arg;
        if (!tok || !tok[0]) {
            return;
        }
        snprintf(sm->store.token, sizeof(sm->store.token), "%s", tok);
        snprintf(sm->store.token_self, sizeof(sm->store.token_self), "%s",
                 sm->st.has_server ? sm->st.server.self : "");
        set_token_addr(sm, sm->st.has_server ? sm->st.server.host : "", sm->st.has_server ? sm->st.server.port : 0);
        clear_pending(sm);
        save(sm);
        sm->error_backoff_ms = ERR_MIN_MS;
        sm->dup_backoff_ms = DUP_MIN_MS;
        set_error(sm, "");
        if (!sm->st.has_server) {
            notify(sm); /* kept; verified as soon as a server is known */
            return;
        }
        if (sm->st.busy) {
            sm->reeval = true; /* verify once the in-flight action has answered */
            notify(sm);
            return;
        }
        do_verify(sm, false);
        return;
    }

    case ESPOS_SK_EV_RETRY:
        if (!sm->started || !sm->st.has_server || sm->st.busy) {
            return;
        }
        sm->error_backoff_ms = ERR_MIN_MS;
        sm->dup_backoff_ms = DUP_MIN_MS;
        set_error(sm, "");
        if (sm->st.state == ESPOS_SK_TOK_DENIED || sm->st.state == ESPOS_SK_TOK_OPEN) {
            clear_pending(sm);
            save(sm);
            do_request(sm);
        } else {
            evaluate(sm);
        }
        return;

    case ESPOS_SK_EV_UNAUTHORIZED:
        if (sm->st.state == ESPOS_SK_TOK_APPROVED && !sm->st.busy) {
            if (handle_unauthorized(sm)) {
                do_request(sm);
            }
        }
        return;

    case ESPOS_SK_EV_CERT_ERROR:
        /* Only from a state that was actually talking to the server. In
         * REQUESTED or IDLE the failing leg reports through its own *_RESULT
         * with cert_error set, and a second path in would double-count. */
        if (sm->st.state == ESPOS_SK_TOK_APPROVED || sm->st.state == ESPOS_SK_TOK_OPEN ||
            sm->st.state == ESPOS_SK_TOK_VERIFYING) {
            sm->st.busy = false;
            enter_cert_error(sm, (const char *)arg);
        }
        return;
    }
}
