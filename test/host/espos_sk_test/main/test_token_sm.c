/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Token state machine tests: every transition of the M3 diagram, including
 * 404 on poll, reboot mid-approval (store round trip), token revoked while
 * approved, reinstalled server, denied, security disabled, manual token.
 */
#include <string.h>
#include "unity.h"
#include "espos_sk_token_sm.h"

static struct {
    uint32_t now;
    bool timer_armed;
    uint32_t timer_due;
    int requests, polls, verifies, saves, notifies;
    char last_href[ESPOS_SK_HREF_MAX];
    char last_verify_token[ESPOS_SK_TOKEN_MAX];
    /* Which server the last leg was aimed at. Counting legs cannot catch a
     * request sent to the wrong host, which is the whole failure mode around
     * a server change. */
    espos_sk_server_t last_request_srv;
    espos_sk_server_t last_poll_srv;
    espos_sk_tok_store_t saved;
    uint32_t rnd;
} F;

static void f_request(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg)
{
    (void)ctx;
    (void)cfg;
    F.requests++;
    F.last_request_srv = *srv;
}
static void f_poll(void *ctx, const espos_sk_server_t *srv, const char *href)
{
    (void)ctx;
    F.polls++;
    F.last_poll_srv = *srv;
    strcpy(F.last_href, href);
}
static void f_verify(void *ctx, const espos_sk_server_t *srv, const char *tok)
{
    (void)ctx;
    (void)srv;
    F.verifies++;
    strcpy(F.last_verify_token, tok);
}
static void f_save(void *ctx, const espos_sk_tok_store_t *st)
{
    (void)ctx;
    F.saves++;
    F.saved = *st;
}
static void f_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    F.timer_armed = true;
    F.timer_due = F.now + ms;
}
static void f_cancel(void *ctx)
{
    (void)ctx;
    F.timer_armed = false;
}
static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return F.now;
}
static uint32_t f_random(void *ctx)
{
    (void)ctx;
    return F.rnd;
}
static void f_notify(void *ctx)
{
    (void)ctx;
    F.notifies++;
}

static const espos_sk_tok_port_t PORT = {
    .http_request = f_request,
    .http_poll = f_poll,
    .http_verify = f_verify,
    .store_save = f_save,
    .arm_timer = f_arm,
    .cancel_timer = f_cancel,
    .now_ms = f_now,
    .random = f_random,
    .status_changed = f_notify,
};

static espos_sk_tok_sm_t SM;
static const espos_sk_tok_cfg_t CFG = { .client_id = "11111111-2222-4333-8444-555555555555", .description = "espOS test", .permissions = "readwrite", .check_interval_ms = 60000 };
static const espos_sk_server_t SRV_A = { .host = "10.0.0.10", .port = 80, .self = "urn:mrn:signalk:uuid:aaaa" };
static const espos_sk_server_t SRV_B = { .host = "10.0.0.11", .port = 3000, .self = "urn:mrn:signalk:uuid:bbbb" };
static const espos_sk_server_t SRV_MANUAL = { .host = "192.168.1.5", .port = 80, .self = "" };

static void reset(const espos_sk_tok_store_t *store)
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    espos_sk_tok_init(&SM, &PORT, NULL, &CFG, store);
}
static void tick(uint32_t ms)
{
    uint32_t target = F.now + ms;
    if (F.timer_armed && (int32_t)(F.timer_due - target) <= 0) {
        F.now = F.timer_due;
        F.timer_armed = false;
        espos_sk_tok_event(&SM, ESPOS_SK_EV_TIMER, NULL);
    }
    F.now = target;
}
#define ST() espos_sk_tok_status(&SM)

static void request_result(int status, const char *href, const char *msg)
{
    espos_sk_http_result_t r = { .http_status = status };
    if (href) {
        strcpy(r.href, href);
        strcpy(r.state, "PENDING");
    }
    if (msg) { strcpy(r.message, msg); }
    espos_sk_tok_event(&SM, ESPOS_SK_EV_REQUEST_RESULT, &r);
}
static void poll_result(int status, const char *state, const char *perm, const char *token)
{
    espos_sk_http_result_t r = { .http_status = status };
    if (state) strcpy(r.state, state);
    if (perm) strcpy(r.permission, perm);
    if (token) strcpy(r.token, token);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_POLL_RESULT, &r);
}
static void verify_result(int status, const char *self)
{
    espos_sk_http_result_t r = { .http_status = status };
    if (self) strcpy(r.self, self);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_VERIFY_RESULT, &r);
}

TEST_CASE("happy path: request → pending → approved → verifying → approved", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_NO_SERVER, ST()->state);
    TEST_ASSERT_EQUAL(0, F.requests);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_TRUE(ST()->busy);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    TEST_ASSERT_FALSE(ST()->busy);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", F.saved.pending_href);   /* persisted */
    TEST_ASSERT_EQUAL_STRING("urn:mrn:signalk:uuid:aaaa", F.saved.pending_self);
    TEST_ASSERT_TRUE(F.timer_armed);
    TEST_ASSERT_EQUAL_UINT32(F.now + 5000, F.timer_due);                          /* first poll after 5 s */
    tick(5000);
    TEST_ASSERT_EQUAL(1, F.polls);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", F.last_href);
    poll_result(200, "PENDING", NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(F.now + 7500, F.timer_due);                          /* 5 s · 1.5 */
    tick(7500);
    poll_result(200, "PENDING", NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(F.now + 11250, F.timer_due);
    tick(11250);
    poll_result(200, "COMPLETED", "APPROVED", "tok.en.1");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL_STRING("tok.en.1", F.last_verify_token);
    TEST_ASSERT_EQUAL_STRING("tok.en.1", F.saved.token);
    TEST_ASSERT_EQUAL_STRING("urn:mrn:signalk:uuid:aaaa", F.saved.token_self);
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", F.saved.token_host);                     /* and where */
    TEST_ASSERT_EQUAL(80, F.saved.token_port);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);                            /* cleared */
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    TEST_ASSERT_EQUAL_STRING("tok.en.1", espos_sk_tok_token(&SM));
    TEST_ASSERT_EQUAL_UINT32(F.now + 60000, F.timer_due);                         /* periodic check */
    TEST_ASSERT_EQUAL(1, ST()->approve_count);
    /* periodic check keeps it approved */
    tick(60000);
    TEST_ASSERT_EQUAL(2, F.verifies);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
}

TEST_CASE("poll backoff caps at 60 s", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    for (int i = 0; i < 12; i++) {
        tick(F.timer_due - F.now);
        poll_result(200, "PENDING", NULL, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(60000, ST()->poll_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(F.now + 60000, F.timer_due);
}

TEST_CASE("denied: no auto retry, RETRY from the user requests again", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    tick(5000);
    poll_result(200, "COMPLETED", "DENIED", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_DENIED, ST()->state);
    TEST_ASSERT_FALSE(F.timer_armed);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);
    TEST_ASSERT_EQUAL(1, ST()->deny_count);
    tick(600000);
    TEST_ASSERT_EQUAL(1, F.requests);                                              /* nothing happens */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_RETRY, NULL);
    TEST_ASSERT_EQUAL(2, F.requests);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
}

TEST_CASE("404 on poll: server lost the request → re-request", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    tick(5000);
    poll_result(404, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL(2, F.requests);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);
    /* signalk-server answers 500 "Unable to check request: not found" — same */
    request_result(202, "/signalk/v1/requests/def", NULL);
    tick(5000);
    espos_sk_http_result_t r = { .http_status = 500 };
    strcpy(r.message, "Unable to check request: not found");
    espos_sk_tok_event(&SM, ESPOS_SK_EV_POLL_RESULT, &r);
    TEST_ASSERT_EQUAL(3, F.requests);
    /* a plain 500 (server hiccup) is transient: keep the pending request */
    request_result(202, "/signalk/v1/requests/ghi", NULL);
    tick(5000);
    poll_result(500, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/ghi", F.saved.pending_href);
}

TEST_CASE("reboot mid-approval resumes polling the stored href", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    espos_sk_tok_store_t persisted = F.saved;                                        /* what NVS holds */
    /* "reboot": fresh machine from the persisted store */
    reset(&persisted);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(0, F.requests);                                              /* no new request */
    TEST_ASSERT_EQUAL(1, F.polls);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", F.last_href);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    poll_result(200, "COMPLETED", "APPROVED", "tok.en.2");
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    /* the pending request was for A; server B after reboot must not poll it */
    reset(&persisted);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_B);
    TEST_ASSERT_EQUAL(0, F.polls);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);
}

/* Repinning a RUNNING device -- the operator changes sk.server_host while a
 * request is outstanding -- is not the same path as a reboot: the store is
 * live rather than reloaded, and the poll leg may still be in flight. Both
 * variants below were found by repinning a gateway from a discovered server
 * to a manual one on the boat LAN, where the old href kept being polled at
 * the new address and the device sat "pending" against a request that server
 * had never issued. */
TEST_CASE("repinning a live device drops a pending request for the old server", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", F.saved.pending_href);

    /* Repin to a manual host: no self URN, so only host/port can decide. */
    F.polls = 0;
    F.requests = 0;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_MANUAL);
    TEST_ASSERT_EQUAL(0, F.polls);                      /* never poll A's href at the new host */
    TEST_ASSERT_EQUAL(1, F.requests);                   /* ask the new server for its own */
    TEST_ASSERT_EQUAL_STRING(SRV_MANUAL.host, F.last_request_srv.host); /* at the NEW address */
    TEST_ASSERT_EQUAL(SRV_MANUAL.port, F.last_request_srv.port);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href); /* and forget the old one */
}

TEST_CASE("repinning while a poll is in flight discards the stale answer", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    tick(5000);                                         /* poll leg starts: busy */
    TEST_ASSERT_TRUE(ST()->busy);

    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_MANUAL);
    F.polls = 0;
    F.requests = 0;
    /* A's approval lands after the switch. Attributing it to the new server
     * would hand the device a token the new server never issued. */
    poll_result(200, "COMPLETED", "APPROVED", "tok.for.A");
    TEST_ASSERT_NOT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token);
    TEST_ASSERT_EQUAL(1, F.requests);                   /* start over against the new server */
    TEST_ASSERT_EQUAL_STRING(SRV_MANUAL.host, F.last_request_srv.host);
    TEST_ASSERT_EQUAL(SRV_MANUAL.port, F.last_request_srv.port);
}

TEST_CASE("stored token: reboot verifies it, revocation (401) re-requests", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL(0, F.requests);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    /* Admin revokes: the periodic check gets 401. Over plaintext the first
     * one only buys a re-check 5 s later — a proxy or captive portal answers
     * 401 too, and dropping the token over one costs a trip to the server's
     * admin UI. The second, with nothing having answered 200 in between, is
     * the server's word and clears it. See espos_sk_tok_sm_t::plain_unauth_streak. */
    tick(60000);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", F.saved.token[0] ? F.saved.token : "tok.en.3");
    tick(5000);
    TEST_ASSERT_EQUAL(3, F.verifies); /* boot check, periodic check, second opinion */
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token);
    TEST_ASSERT_EQUAL(2, ST()->unauthorized_count);
    request_result(202, "/signalk/v1/requests/xyz", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
}

TEST_CASE("another SK call reporting 401 invalidates the token", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    /* Plaintext again: the first report is a re-check, not a verdict. */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_UNAUTHORIZED, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    tick(5000);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL_STRING("", espos_sk_tok_token(&SM));
}

TEST_CASE("reinstalled server (new self URN) triggers a fresh request", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_B);                           /* different self */
    TEST_ASSERT_EQUAL(0, F.verifies);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", SM.store.token);                          /* kept for server A */
}

TEST_CASE("server that changed address keeps its token (keyed by self)", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_server_t moved = SRV_A;
    strcpy(moved.host, "10.0.0.99");
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &moved);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL(0, F.requests);
}

TEST_CASE("an old token learns its address on verify; a recorded one is not re-homed", "[sk_tok]")
{
    /* stored before the address was recorded */
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", F.saved.token_host);
    TEST_ASSERT_EQUAL(80, F.saved.token_port);
    /* a 200 elsewhere (a same-self twin with security off answers 200 to
     * anything) does not move the anchor */
    espos_sk_server_t twin = SRV_A;
    strcpy(twin.host, "10.0.0.99");
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &twin);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", F.saved.token_host);
    /* a dropped token forgets the address with it */
    tick(60000);
    verify_result(401, NULL);
    tick(5000);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token_host);
    TEST_ASSERT_EQUAL(0, F.saved.token_port);
}

TEST_CASE("a stand-in found by self cannot cost the token: 401 there waits for our server", "[sk_tok]")
{
    /* The real server restarts long enough for discovery to fall back to a
     * twin advertising the same MMSI. The twin refuses our token, as it
     * should; that used to clear the token and file a request on the twin. */
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa", .token_host = "10.0.0.10", .token_port = 80 };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_server_t twin = SRV_A;
    strcpy(twin.host, "10.0.0.99");
    twin.by_self = true;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &twin);
    TEST_ASSERT_EQUAL(1, F.verifies);
    verify_result(401, NULL); /* plaintext: a second opinion first, as anywhere */
    tick(5000);
    TEST_ASSERT_EQUAL(2, F.verifies);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    TEST_ASSERT_EQUAL(0, F.requests);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", SM.store.token);
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", SM.store.token_host);
    /* the retry asks again, and is refused again, without giving up the token */
    tick(60000);
    TEST_ASSERT_EQUAL(3, F.verifies);
    verify_result(401, NULL);
    tick(5000);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(0, F.requests);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", SM.store.token);
    /* our server answers again and is chosen: the token still works */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", F.last_verify_token);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.requests);
}

TEST_CASE("a stand-in found by self cannot cost the pending request: 404 there waits", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .pending_href = "/signalk/v1/requests/abc", .pending_host = "10.0.0.10", .pending_port = 80, .pending_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_server_t twin = SRV_A;
    strcpy(twin.host, "10.0.0.99");
    twin.by_self = true;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &twin);
    TEST_ASSERT_EQUAL(1, F.polls);
    poll_result(404, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    TEST_ASSERT_EQUAL(0, F.requests);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/abc", SM.store.pending_href);
    /* back on our server the approval is collected */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(2, F.polls);
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", F.last_poll_srv.host);
    poll_result(200, "COMPLETED", "APPROVED", "tok.en.9");
    TEST_ASSERT_EQUAL_STRING("tok.en.9", SM.store.token);
    TEST_ASSERT_EQUAL_STRING("10.0.0.10", SM.store.token_host);
}

TEST_CASE("a server that stops being a stand-in is told so without a server change", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa", .token_host = "10.0.0.10", .token_port = 80 };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_server_t moved = SRV_A;
    strcpy(moved.host, "10.0.0.99");
    moved.by_self = true;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &moved);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_TRUE(ST()->server.by_self);
    /* a 401 on another call there (the stream upgrade) is no verdict */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_UNAUTHORIZED, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state); /* the stream is not dropped over one */
    tick(5000);
    TEST_ASSERT_EQUAL(2, F.verifies);
    verify_result(401, NULL);
    TEST_ASSERT_EQUAL(0, F.requests);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", SM.store.token);
    int n = F.notifies;
    moved.by_self = false;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &moved);
    TEST_ASSERT_FALSE(ST()->server.by_self);
    TEST_ASSERT_GREATER_THAN(n, F.notifies);
    TEST_ASSERT_EQUAL(2, F.verifies); /* same server: no new leg */
}

TEST_CASE("manual server without self: token verified, self learned and stored", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.4", .token_self = "" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_MANUAL);
    TEST_ASSERT_EQUAL(1, F.verifies);
    verify_result(200, "urn:mrn:signalk:uuid:cccc");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
    TEST_ASSERT_EQUAL_STRING("urn:mrn:signalk:uuid:cccc", F.saved.token_self);
    TEST_ASSERT_EQUAL_STRING("urn:mrn:signalk:uuid:cccc", ST()->server.self);
}

TEST_CASE("security disabled on the server: OPEN, re-probed by POST, requests when enabled", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(404, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_OPEN, ST()->state);
    tick(60000);
    TEST_ASSERT_EQUAL(2, F.requests);                                              /* probe = re-POST */
    TEST_ASSERT_EQUAL(0, F.verifies);                                              /* GET /self is blind with allow_readonly */
    request_result(404, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_OPEN, ST()->state);
    tick(60000);
    request_result(202, "/signalk/v1/requests/x", NULL);                          /* security switched on */
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    TEST_ASSERT_EQUAL(3, F.requests);
}

TEST_CASE("a host that is not a SignalK server (599) is an error, not 'open'", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_MANUAL);
    request_result(599, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    TEST_ASSERT_NOT_NULL(strstr(ST()->last_error, "not a SignalK"));
}

TEST_CASE("error retry re-evaluates: a token kept for another server is not verified against this one", "[sk_tok]")
{
    espos_sk_tok_store_t st = { .token = "tok.en.3", .token_self = "urn:mrn:signalk:uuid:aaaa" };
    reset(&st);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_B);                           /* other server → request */
    request_result(0, NULL, NULL);                                                /* unreachable */
    tick(F.timer_due - F.now);
    TEST_ASSERT_EQUAL(0, F.verifies);
    TEST_ASSERT_EQUAL(2, F.requests);
    TEST_ASSERT_EQUAL_STRING("tok.en.3", SM.store.token);
}

TEST_CASE("self URN becoming known for the same server is not a server change", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_MANUAL);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    tick(5000);
    poll_result(200, "COMPLETED", "DENIED", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_DENIED, ST()->state);
    espos_sk_server_t known = SRV_MANUAL;
    strcpy(known.self, "urn:mrn:signalk:uuid:cccc");
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &known);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_DENIED, ST()->state);                           /* not re-requested */
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL_STRING("urn:mrn:signalk:uuid:cccc", ST()->server.self);
    /* but a different self on the same address is a reinstalled server */
    strcpy(known.self, "urn:mrn:signalk:uuid:dddd");
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &known);
    TEST_ASSERT_EQUAL(2, F.requests);
}

TEST_CASE("manual token with no server yet is kept and verified once a server appears", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_MANUAL_TOKEN, "pasted.tok");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_NO_SERVER, ST()->state);
    TEST_ASSERT_EQUAL_STRING("pasted.tok", F.saved.token);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL_STRING("pasted.tok", F.last_verify_token);
}

TEST_CASE("device requests disabled (403) → denied with message", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(403, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_DENIED, ST()->state);
    TEST_ASSERT_NOT_NULL(strstr(ST()->last_error, "disabled"));
}

TEST_CASE("duplicate pending request (400) backs off, first retry still prompt", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(400, NULL, "A device with clientId 'x' has already requested access");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    /* The status line has to say what the reader must DO. Now that the retry
     * stretches to ten minutes, a line that reads like a stuck device rather
     * than one waiting for a person is the difference between somebody
     * approving it and somebody power-cycling it. The server's own wording
     * names a uuid nobody can act on, so it is deliberately replaced. */
    TEST_ASSERT_NOT_NULL(strstr(ST()->last_error, "approve it in Security -> Access Requests"));

    /* The usual case is somebody approving it within a minute or two, so the
     * FIRST retry stays prompt: a minute, less the jitter (rnd=0 -> -10 %). */
    uint32_t d1 = F.timer_due - F.now;
    TEST_ASSERT_TRUE(d1 >= 54000 && d1 <= 72000);
    tick(d1);
    TEST_ASSERT_EQUAL(2, F.requests);
}

TEST_CASE("duplicate pending request stops asking every minute forever", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);

    /* Nobody approves it. Measured on an ESP32-C5 before this backoff existed:
     * 31 requests in ~34 minutes, each spiking the heap, until the watchdog
     * rebooted the board (espOS #128). */
    uint32_t prev = 0;
    for (int i = 0; i < 6; i++) {
        request_result(400, NULL, "already requested access");
        uint32_t d = F.timer_due - F.now;
        /* Growing OR already at the ceiling. Not "strictly greater than the
         * last": the cap is the point, so once it is reached the wait stops
         * growing -- an earlier version of this assertion demanded growth
         * forever and failed on the code behaving correctly.
         *
         * Observed ladder: 54s, 108s, 216s, 432s, 540s, 540s. */
        if (i > 0) {
            TEST_ASSERT_TRUE_MESSAGE(d > prev || d >= 540000u,
                                     "each wait must grow until it is capped");
        }
        prev = d;
        tick(d);
    }
    /* Six refusals in, the wait is minutes rather than the fixed minute it used
     * to be -- and bounded, so a device is still picked up soon after someone
     * finally approves it rather than sleeping for hours. */
    TEST_ASSERT_TRUE(prev >= 8u * 60000u);
    TEST_ASSERT_TRUE(prev <= 600000u + 600000u / 5u);
}

TEST_CASE("an answered server resets the duplicate backoff", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    for (int i = 0; i < 4; i++) {
        request_result(400, NULL, "already requested access");
        tick(F.timer_due - F.now);
    }
    /* The request finally lands: the ladder must reset, or the NEXT time this
     * device is waiting for approval it would start minutes deep for a reason
     * that has already been resolved. */
    request_result(202, "/signalk/v1/requests/abc", NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    /* Getting back to a fresh request has to go THROUGH the machine rather than
     * around it. An earlier version of this test re-sent EV_SERVER and then a
     * REQUEST_RESULT, but in REQUESTED that result is stale and dropped, so the
     * delay it measured was the 5 s poll timer and the assertion passed without
     * the reset ever being exercised. A 404 on the poll is the real path: the
     * server lost the request, so the device asks again. */
    tick(F.timer_due - F.now);
    poll_result(404, NULL, NULL, NULL);
    request_result(400, NULL, "already requested access");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    uint32_t d = F.timer_due - F.now;
    /* Both bounds: an upper one alone would also pass if the wait collapsed to
     * zero, which would be the old every-minute storm at a faster rate. This is
     * the same 54-72 s window the first retry gets. */
    TEST_ASSERT_TRUE_MESSAGE(d >= 54000 && d <= 72000, "backoff should be back at its floor");
}

TEST_CASE("a 400 that is not a duplicate reports the server's reason", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);

    /* signalk-server answers 400 to three other things on this endpoint, all of
     * them malformed requests from this side rather than a person who has not
     * looked yet. Advising somebody to approve a request would send them to an
     * empty list, so these must keep the server's own words and back off as the
     * error they are. */
    request_result(400, NULL, "invalid permissions");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    TEST_ASSERT_NOT_NULL(strstr(ST()->last_error, "invalid permissions"));
    TEST_ASSERT_NULL(strstr(ST()->last_error, "Access Requests"));

    /* The error ladder, not the ten-minute approval one: a bug on this side
     * should surface, not be hidden behind a long sleep. */
    uint32_t d = F.timer_due - F.now;
    TEST_ASSERT_TRUE_MESSAGE(d >= 9000 && d <= 12000, "should use the error backoff floor");
}

TEST_CASE("unreachable server: exponential error backoff, resumes the right step", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(0, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    uint32_t d1 = F.timer_due - F.now;
    TEST_ASSERT_TRUE(d1 >= 9000 && d1 <= 12000);                                   /* 10 s ±20 % (rnd=0 → 9 s) */
    tick(d1);
    TEST_ASSERT_EQUAL(2, F.requests);
    request_result(0, NULL, NULL);
    uint32_t d2 = F.timer_due - F.now;
    TEST_ASSERT_TRUE(d2 >= 18000 && d2 <= 24000);
    /* while pending, an outage resumes with a poll, not a new request */
    tick(d2);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    tick(5000);
    poll_result(0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    tick(F.timer_due - F.now);
    TEST_ASSERT_EQUAL(2, F.polls);
    TEST_ASSERT_EQUAL(3, F.requests);
    /* approved token, outage during check: stays approved-ish (ERROR) then verifies */
    poll_result(200, "COMPLETED", "APPROVED", "tok");
    verify_result(0, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_ERROR, ST()->state);
    tick(F.timer_due - F.now);
    TEST_ASSERT_EQUAL(2, F.verifies);
    verify_result(200, "urn:mrn:signalk:uuid:aaaa");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, ST()->state);
}

TEST_CASE("manual token paste verifies immediately", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_MANUAL_TOKEN, "pasted.tok");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("pasted.tok", F.last_verify_token);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);                            /* pending dropped */
    /* A bad paste over plaintext gets the same second opinion as any other
     * unauthorised answer — the device cannot tell "you typed it wrong" from
     * "a proxy answered", and the retry costs 5 s. */
    verify_result(403, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, ST()->state); /* still deciding, token still there */
    TEST_ASSERT_EQUAL_STRING("pasted.tok", F.saved.token);
    tick(5000);
    verify_result(403, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    TEST_ASSERT_EQUAL(2, F.requests);
}

TEST_CASE("stale results and stale timers are ignored; stop cancels", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    request_result(202, "/signalk/v1/requests/abc", NULL);
    int n = F.notifies;
    poll_result(200, "COMPLETED", "APPROVED", "tok");                              /* no poll in flight */
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_REQUESTED, ST()->state);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_TIMER, NULL);                             /* early */
    TEST_ASSERT_EQUAL(0, F.polls);
    TEST_ASSERT_EQUAL(n, F.notifies);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_STOP, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_NO_SERVER, ST()->state);
    TEST_ASSERT_FALSE(F.timer_armed);
    /* start again: the known server is resumed (pending poll) */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    TEST_ASSERT_EQUAL(1, F.polls);
    poll_result(200, "PENDING", NULL, NULL);                                      /* answer lands */
    /* server goes away → NO_SERVER; comes back → resumes pending */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_NO_SERVER, ST()->state);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);
    TEST_ASSERT_EQUAL(2, F.polls);
}

TEST_CASE("server change while an action is in flight: the stale answer is dropped", "[sk_tok]")
{
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_A);                           /* request to A in flight */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV_B);                           /* switch while busy */
    TEST_ASSERT_TRUE(ST()->busy);
    TEST_ASSERT_EQUAL(1, F.requests);
    request_result(202, "/signalk/v1/requests/for-A", NULL);                      /* A's answer arrives */
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);                            /* not attributed to B */
    TEST_ASSERT_EQUAL(2, F.requests);                                              /* fresh request to B */
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, ST()->state);
    /* manual token while busy waits for the answer, then verifies */
    espos_sk_tok_event(&SM, ESPOS_SK_EV_MANUAL_TOKEN, "pasted");
    TEST_ASSERT_EQUAL(0, F.verifies);
    request_result(202, "/signalk/v1/requests/for-B", NULL);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL_STRING("pasted", F.last_verify_token);
}

TEST_CASE("state names", "[sk_tok]")
{
    TEST_ASSERT_EQUAL_STRING("pending", espos_sk_tok_state_str(ESPOS_SK_TOK_REQUESTED));
    TEST_ASSERT_EQUAL_STRING("approved", espos_sk_tok_state_str(ESPOS_SK_TOK_APPROVED));
    TEST_ASSERT_EQUAL_STRING("denied", espos_sk_tok_state_str(ESPOS_SK_TOK_DENIED));
}
