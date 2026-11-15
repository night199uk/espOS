/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The trust decision table (espos_sk_tls_policy.h) and the SAN normalisation,
 * plus the token machine's certificate states — the parts of S1 that are pure
 * logic and can be driven directly, without mbedTLS or a server.
 *
 * What these are guarding: every one of them is a way to get the trust
 * decision subtly wrong in a direction that accepts too much. A CA anchor that
 * ignores the SAN set trusts every host the CA ever signs; a truncated SAN set
 * that compares equal accepts a certificate naming fewer hosts than the real
 * one; a first-use capture that prefers the CA when the leaf is anonymous
 * widens the anchor past what the operator agreed to.
 */
#include <string.h>
#include "unity.h"
#include "espos_sk_tls_policy.h"
#include "espos_sk_token_sm.h"

/* ------------------------------------------------------- decision table */

static espos_sk_tls_decision_t decide(bool has_anchor, bool anchor_is_leaf, bool leaf_matches, bool ca_present,
                                      bool ca_matches, bool leaf_has_san, bool san_matches)
{
    espos_sk_tls_facts_t f = {
        .has_anchor = has_anchor,
        .anchor_is_leaf = anchor_is_leaf,
        .leaf_matches = leaf_matches,
        .ca_present = ca_present,
        .ca_matches = ca_matches,
        .leaf_has_san = leaf_has_san,
        .san_matches = san_matches,
    };
    return espos_sk_tls_decide(&f);
}

TEST_CASE("first use with a CA and a named leaf anchors at the CA", "[sk_tls]")
{
    /* The renewal-survivable shape: the CA says who signed it, the SAN set
     * says for whom, and a certificate renewed by the same CA for the same
     * names is accepted with nobody pressing a button. */
    espos_sk_tls_decision_t d = decide(false, false, false, true, false, true, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CAPTURE, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_FIRST_USE, d.reason);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ANCHOR_CA, d.capture_as);
}

TEST_CASE("first use without a CA anchors at the leaf", "[sk_tls]")
{
    /* signalk-server's own generated self-signed certificate: one certificate,
     * no CA above it. There is nothing to pin but the certificate itself. */
    espos_sk_tls_decision_t d = decide(false, false, false, false, false, true, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CAPTURE, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ANCHOR_LEAF, d.capture_as);
}

TEST_CASE("first use with a CA but an anonymous leaf anchors at the leaf", "[sk_tls]")
{
    /* A CA anchor here would vouch for every name that CA ever signs, because
     * there is no SAN set to hold the renewal to. Narrower is right. */
    espos_sk_tls_decision_t d = decide(false, false, false, true, false, false, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CAPTURE, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ANCHOR_LEAF, d.capture_as);
}

TEST_CASE("leaf anchor: same certificate accepted, any other refused", "[sk_tls]")
{
    espos_sk_tls_decision_t d = decide(true, true, true, false, false, true, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ACCEPT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_OK, d.reason);
    /* Renewed by the same CA is still a different certificate under a leaf
     * anchor: that is what makes the anchor strict, and why an operator has
     * to say so once after a renewal. */
    d = decide(true, true, false, true, true, true, true);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_LEAF_CHANGED, d.reason);
}

TEST_CASE("CA anchor: renewal by the same CA for the same names is accepted", "[sk_tls]")
{
    /* The whole point of the CA shape: leaf_matches is false — this is a
     * certificate the device has never seen — and it is accepted anyway. */
    espos_sk_tls_decision_t d = decide(true, false, false, true, true, true, true);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ACCEPT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_OK, d.reason);
}

TEST_CASE("CA anchor: a different CA, other names, or no name is refused", "[sk_tls]")
{
    espos_sk_tls_decision_t d = decide(true, false, false, true, false, true, true);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_CA_CHANGED, d.reason);
    /* A chain that lost its CA entirely reads the same way: we cannot show
     * this leaf came from the CA we pinned. */
    d = decide(true, false, false, false, false, true, true);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_CA_CHANGED, d.reason);
    /* Right CA, different hosts: the private CA signed something else. This
     * is the case a CA-only anchor would wave through. */
    d = decide(true, false, false, true, true, true, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_SAN_CHANGED, d.reason);
    /* Right CA, leaf says nothing about who it is: indistinguishable from
     * "some other host the same CA signed". */
    d = decide(true, false, false, true, true, false, false);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_NO_IDENTITY, d.reason);
}

TEST_CASE("a forged link under the pinned CA is refused, and on first use pins the leaf", "[sk_tls]")
{
    /* The pinned CA's certificate is public, so a server can hang it over a
     * leaf of its own: the CA fingerprint and even the names then match. */
    espos_sk_tls_facts_t f = {
        .has_anchor = true,
        .ca_present = true,
        .ca_matches = true,
        .leaf_has_san = true,
        .san_matches = true,
        .chain_forged = true,
    };
    espos_sk_tls_decision_t d = espos_sk_tls_decide(&f);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_CHAIN_FORGED, d.reason);
    TEST_ASSERT_NOT_NULL(strstr(espos_sk_tls_reason_str(d.reason), "pinned CA"));

    /* A different CA stays CA_CHANGED, forged or not. */
    f.ca_matches = false;
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_R_CA_CHANGED, espos_sk_tls_decide(&f).reason);

    /* A leaf anchor holds the exact certificate, so the chain does not matter. */
    f.anchor_is_leaf = true;
    f.leaf_matches = true;
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ACCEPT, espos_sk_tls_decide(&f).verdict);

    /* First use: nothing is pinned yet, so accept, but never anchor at a CA
     * that did not sign the chain. */
    f = (espos_sk_tls_facts_t) { .ca_present = true, .leaf_has_san = true, .chain_forged = true };
    d = espos_sk_tls_decide(&f);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CAPTURE, d.verdict);
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_ANCHOR_LEAF, d.capture_as);
}

TEST_CASE("every reason has a sentence", "[sk_tls]")
{
    for (int r = ESPOS_SK_TLS_R_OK; r <= ESPOS_SK_TLS_R_CHAIN_FORGED; r++) {
        const char *s = espos_sk_tls_reason_str((espos_sk_tls_reason_t)r);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE(s[0] != '\0');
    }
    TEST_ASSERT_NOT_NULL(espos_sk_tls_reason_str((espos_sk_tls_reason_t)99));
    /* A NULL facts pointer must not be an accept. */
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_REJECT, espos_sk_tls_decide(NULL).verdict);
}

/* ---------------------------------------------------- SAN normalisation */

TEST_CASE("SAN sets normalise to one sorted, lower-cased, de-duplicated string", "[sk_tls]")
{
    const char *in[] = { "Boat.local", "192.168.1.10", "boat.local", "SIGNALK.lan" };
    char out[128];
    bool cut = true;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sk_tls_san_normalise(in, 4, out, sizeof(out), &cut));
    TEST_ASSERT_FALSE(cut);
    /* Sorted so a certificate that lists the same names in another order —
     * which a re-issue routinely does — still compares equal. */
    TEST_ASSERT_EQUAL_STRING("192.168.1.10,boat.local,signalk.lan", out);
    TEST_ASSERT_TRUE(espos_sk_tls_san_equal(out, "192.168.1.10,boat.local,signalk.lan"));
}

TEST_CASE("an empty SAN set is empty, and never equal to anything", "[sk_tls]")
{
    char out[64] = "dirty";
    bool cut = true;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sk_tls_san_normalise(NULL, 0, out, sizeof(out), &cut));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(cut);
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal("", ""));
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal("boat.local", ""));
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal(NULL, "boat.local"));
}

TEST_CASE("an empty stored SAN set binds the first leaf; a stored one must match", "[sk_tls]")
{
    /* sk.ca_pem anchors the CA before any handshake has named the server, so
     * the stored set is empty. This is what lets the first leaf through; the
     * commit then stores its set and later connections are held to it. */
    TEST_ASSERT_TRUE(espos_sk_tls_san_matches_anchor("", "boat.local"));
    TEST_ASSERT_TRUE(espos_sk_tls_san_matches_anchor(NULL, "boat.local"));
    TEST_ASSERT_TRUE(espos_sk_tls_san_matches_anchor("boat.local", "boat.local"));
    TEST_ASSERT_FALSE(espos_sk_tls_san_matches_anchor("boat.local", "other.local"));
    TEST_ASSERT_FALSE(espos_sk_tls_san_matches_anchor("boat.local", ""));
    TEST_ASSERT_FALSE(espos_sk_tls_san_matches_anchor("!boat.local", "boat.local"));
}

TEST_CASE("a SAN set that does not fit is refused, not silently shortened", "[sk_tls]")
{
    /* The dangerous failure: storing the two names that fitted, then later
     * matching a certificate that names only those two. */
    const char *in[] = { "aaaaaaaaaa.example", "bbbbbbbbbb.example", "cccccccccc.example" };
    char out[24];
    bool cut = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_sk_tls_san_normalise(in, 3, out, sizeof(out), &cut));
    TEST_ASSERT_TRUE(cut);
    /* And the marker the device stores for it never compares equal. */
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal("!aaaaaaaaaa.example", "!aaaaaaaaaa.example"));
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal("!aaaaaaaaaa.example", "aaaaaaaaaa.example"));
}

TEST_CASE("a set that exactly fills the buffer is not truncated", "[sk_tls]")
{
    const char *in[] = { "ab", "cd" };
    char out[6]; /* "ab,cd" + NUL */
    bool cut = true;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sk_tls_san_normalise(in, 2, out, sizeof(out), &cut));
    TEST_ASSERT_EQUAL_STRING("ab,cd", out);
    TEST_ASSERT_FALSE(cut);
}

/* ------------------------------------------ the token machine's cert states */

static struct {
    uint32_t now;
    bool timer_armed;
    uint32_t timer_due;
    int requests, polls, verifies, saves;
    espos_sk_tok_store_t saved;
} G;

static void g_request(void *c, const espos_sk_server_t *s, const espos_sk_tok_cfg_t *f)
{
    (void)c;
    (void)s;
    (void)f;
    G.requests++;
}
static void g_poll(void *c, const espos_sk_server_t *s, const char *h)
{
    (void)c;
    (void)s;
    (void)h;
    G.polls++;
}
static void g_verify(void *c, const espos_sk_server_t *s, const char *t)
{
    (void)c;
    (void)s;
    (void)t;
    G.verifies++;
}
static void g_save(void *c, const espos_sk_tok_store_t *st)
{
    (void)c;
    G.saves++;
    G.saved = *st;
}
static void g_arm(void *c, uint32_t ms)
{
    (void)c;
    G.timer_armed = true;
    G.timer_due = G.now + ms;
}
static void g_cancel(void *c)
{
    (void)c;
    G.timer_armed = false;
}
static uint32_t g_now(void *c)
{
    (void)c;
    return G.now;
}
static uint32_t g_random(void *c)
{
    (void)c;
    return 0;
}

static const espos_sk_tok_port_t GPORT = {
    .http_request = g_request,
    .http_poll = g_poll,
    .http_verify = g_verify,
    .store_save = g_save,
    .arm_timer = g_arm,
    .cancel_timer = g_cancel,
    .now_ms = g_now,
    .random = g_random,
};

static espos_sk_tok_sm_t TSM;
static const espos_sk_tok_cfg_t TCFG = { .client_id = "cid", .description = "d", .permissions = "readwrite", .check_interval_ms = 60000 };
static const espos_sk_server_t SRV_PLAIN = { .host = "10.0.0.10", .port = 80, .tls = false, .self = "urn:a" };
static const espos_sk_server_t SRV_TLS = { .host = "10.0.0.10", .port = 443, .tls = true, .self = "urn:a" };

static void treset(const espos_sk_tok_store_t *store)
{
    memset(&G, 0, sizeof(G));
    G.now = 1000;
    espos_sk_tok_init(&TSM, &GPORT, NULL, &TCFG, store);
}
static void ttick(uint32_t ms)
{
    uint32_t target = G.now + ms;
    if (G.timer_armed && (int32_t)(G.timer_due - target) <= 0) {
        G.now = G.timer_due;
        G.timer_armed = false;
        espos_sk_tok_event(&TSM, ESPOS_SK_EV_TIMER, NULL);
    }
    G.now = target;
}
#define TST() espos_sk_tok_status(&TSM)

static espos_sk_tok_store_t with_token(const char *tok)
{
    espos_sk_tok_store_t st = { 0 };
    strcpy(st.token, tok);
    strcpy(st.token_self, "urn:a");
    return st;
}

static void cert_result(espos_sk_tok_event_t ev)
{
    espos_sk_http_result_t r = { .http_status = 0, .cert_error = true };
    strcpy(r.cert_reason, "the server certificate changed");
    espos_sk_tok_event(&TSM, ev, &r);
}

TEST_CASE("a certificate refusal on the request leg is cert_error, not error", "[sk_tok]")
{
    treset(NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_TLS);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, TST()->state);
    cert_result(ESPOS_SK_EV_REQUEST_RESULT);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_CERT_ERROR, TST()->state);
    TEST_ASSERT_EQUAL_STRING("cert_error", espos_sk_tok_state_str(TST()->state));
    TEST_ASSERT_EQUAL_STRING("the server certificate changed", TST()->last_error);
    TEST_ASSERT_EQUAL(1, TST()->cert_error_count);
    /* Flat 60 s, not the exponential ladder an unreachable server gets: the
     * fix arrives from outside and should be noticed within a minute. */
    TEST_ASSERT_TRUE(G.timer_armed);
    TEST_ASSERT_EQUAL_UINT32(G.now + 60000, G.timer_due);
}

TEST_CASE("a certificate refusal on the poll leg is cert_error and keeps the request", "[sk_tok]")
{
    treset(NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_TLS);
    espos_sk_http_result_t ok = { .http_status = 202 };
    strcpy(ok.href, "/signalk/v1/requests/x");
    strcpy(ok.state, "PENDING");
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_REQUEST_RESULT, &ok);
    ttick(5000);
    TEST_ASSERT_EQUAL(1, G.polls);
    cert_result(ESPOS_SK_EV_POLL_RESULT);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_CERT_ERROR, TST()->state);
    TEST_ASSERT_EQUAL_STRING("/signalk/v1/requests/x", G.saved.pending_href); /* nothing thrown away */
}

TEST_CASE("a certificate refusal on the verify leg keeps the token", "[sk_tok]")
{
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_PLAIN); /* plaintext: the verify leg runs */
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, TST()->state);
    cert_result(ESPOS_SK_EV_VERIFY_RESULT);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_CERT_ERROR, TST()->state);
    /* The credential is fine; the transport is not. Dropping it would mean a
     * fresh approval in the server's admin UI after every renewal. */
    TEST_ASSERT_TRUE(TST()->has_token);
    TEST_ASSERT_EQUAL_STRING("tok.1", TSM.store.token);
}

TEST_CASE("cert_error retries on the timer and recovers", "[sk_tok]")
{
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_PLAIN);
    cert_result(ESPOS_SK_EV_VERIFY_RESULT);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_CERT_ERROR, TST()->state);
    int before = G.verifies;
    ttick(60000);
    /* The only way to learn the certificate is acceptable now is to try. */
    TEST_ASSERT_EQUAL(before + 1, G.verifies);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, TST()->state);
    espos_sk_http_result_t good = { .http_status = 200 };
    strcpy(good.self, "urn:a");
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &good);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);
}

TEST_CASE("EV_CERT_ERROR from the stream moves an approved device to cert_error", "[sk_tok]")
{
    /* The WebSocket upgrade is where a TLS device most often meets the new
     * certificate, and it reports through this event rather than a leg. */
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_TLS);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state); /* TLS skips the verify leg */
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_CERT_ERROR, "the server certificate changed");
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_CERT_ERROR, TST()->state);
    TEST_ASSERT_TRUE(TST()->has_token);
    /* And a reset-and-retry (DELETE /sk/tls) puts it straight back. */
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_RETRY, NULL);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);
}

TEST_CASE("over TLS the verify leg is skipped: the upgrade checks the token", "[sk_tok]")
{
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_TLS);
    TEST_ASSERT_EQUAL(0, G.verifies); /* no second handshake per reconnect */
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);
    TEST_ASSERT_EQUAL_STRING("tok.1", espos_sk_tok_token(&TSM));
    /* Plaintext still verifies: the leg is nearly free there. */
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_PLAIN);
    TEST_ASSERT_EQUAL(1, G.verifies);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, TST()->state);
}

TEST_CASE("over plaintext one 401 keeps the token, two clear it", "[sk_tok]")
{
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_PLAIN);
    espos_sk_http_result_t good = { .http_status = 200 };
    strcpy(good.self, "urn:a");
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &good);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);

    /* A captive portal, a proxy, a router's login page: anything on a
     * plaintext path can answer 401, and dropping the token over one costs a
     * trip to the server's admin UI to approve the device again. */
    espos_sk_http_result_t unauth = { .http_status = 401 };
    ttick(60000); /* the periodic re-check */
    TEST_ASSERT_EQUAL(2, G.verifies);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &unauth);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);
    TEST_ASSERT_TRUE(TST()->has_token);
    TEST_ASSERT_EQUAL_STRING("tok.1", TSM.store.token);
    TEST_ASSERT_EQUAL(0, G.requests);
    /* ... and a re-check follows shortly, not after the whole check interval. */
    TEST_ASSERT_EQUAL_UINT32(G.now + 5000, G.timer_due);
    ttick(5000);
    TEST_ASSERT_EQUAL(3, G.verifies);

    /* The second refusal in a row is conclusive. */
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &unauth);
    TEST_ASSERT_FALSE(TST()->has_token);
    TEST_ASSERT_EQUAL_STRING("", TSM.store.token);
    TEST_ASSERT_EQUAL(1, G.requests); /* straight into a fresh access request */
}

TEST_CASE("a 200 between two plaintext 401s resets the streak", "[sk_tok]")
{
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_PLAIN);
    espos_sk_http_result_t good = { .http_status = 200 };
    strcpy(good.self, "urn:a");
    espos_sk_http_result_t unauth = { .http_status = 401 };
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &good);
    ttick(60000);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &unauth); /* first */
    ttick(5000);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &good);   /* it was transient */
    ttick(60000);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_VERIFY_RESULT, &unauth); /* first again, not second */
    TEST_ASSERT_TRUE(TST()->has_token);
    TEST_ASSERT_EQUAL_STRING("tok.1", TSM.store.token);
}

TEST_CASE("over TLS one 401 clears the token immediately", "[sk_tok]")
{
    /* Nothing on the path can inject an answer inside TLS, so a 401 is the
     * server's and there is nothing to double-check. */
    espos_sk_tok_store_t st = with_token("tok.1");
    treset(&st);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_SERVER, &SRV_TLS);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_APPROVED, TST()->state);
    espos_sk_tok_event(&TSM, ESPOS_SK_EV_UNAUTHORIZED, NULL);
    TEST_ASSERT_FALSE(TST()->has_token);
    TEST_ASSERT_EQUAL_STRING("", TSM.store.token);
    TEST_ASSERT_EQUAL(1, G.requests);
}

TEST_CASE("reloading the configured CA keeps an anchor already on that CA", "[sk_tls]")
{
    /* sk.ca_pem is loaded again at every boot. Re-creating the anchor then
     * would drop the SAN set the first connection bound, and the next leaf
     * the CA signed, for any name, would bind afresh. */
    uint8_t ca[ESPOS_SK_TLS_FP_LEN], other[ESPOS_SK_TLS_FP_LEN];
    memset(ca, 0xA5, sizeof(ca));
    memset(other, 0xA5, sizeof(other));
    other[ESPOS_SK_TLS_FP_LEN - 1] ^= 1;

    /* The boot reload: the anchor the first connection bound stays. */
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_KEEP, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_CA, ca, ca));
    /* A different CA, a TOFU leaf pin, nothing stored, or nothing to compare:
     * the configured CA becomes a fresh anchor. */
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_REANCHOR, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_CA, other, ca));
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_REANCHOR, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_LEAF, ca, ca));
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_REANCHOR, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_NONE, ca, ca));
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_REANCHOR, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_CA, NULL, ca));
    TEST_ASSERT_EQUAL(ESPOS_SK_TLS_CA_REANCHOR, espos_sk_tls_ca_load(ESPOS_SK_TLS_ANCHOR_CA, ca, NULL));
}

/* ------------------------------------------------------ SAN collection */

static void san_add_str(espos_sk_tls_san_builder_t *b, unsigned tag, const char *s, size_t len)
{
    espos_sk_tls_san_add(b, tag, (const uint8_t *)s, len);
}

TEST_CASE("SAN entries become one set: names and IPs kept, other types skipped", "[sk_tls]")
{
    espos_sk_tls_san_builder_t b = { 0 };
    char out[ESPOS_SK_TLS_SAN_MAX];
    bool cut = true;
    const uint8_t ip4[4] = { 192, 168, 1, 10 };
    const uint8_t ip6[16] = { 0xfe, 0x80, [15] = 1 };
    san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, "Boat.local", 10);
    espos_sk_tls_san_add(&b, ESPOS_SK_TLS_SAN_TAG_IP, ip4, sizeof(ip4));
    espos_sk_tls_san_add(&b, ESPOS_SK_TLS_SAN_TAG_IP, ip6, sizeof(ip6));
    espos_sk_tls_san_add(&b, ESPOS_SK_TLS_SAN_TAG_IP, ip4, 3); /* not an address: skipped */
    san_add_str(&b, 6, "https://boat.local/", 19);               /* a URI: skipped */
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_FALSE(cut);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10,boat.local,fe80:0000:0000:0000:0000:0000:0000:0001", out);
}

TEST_CASE("a 63-byte name fits; a 64-byte name or an embedded NUL cuts the set", "[sk_tls]")
{
    char name[65];
    memset(name, 'a', sizeof(name));
    char out[ESPOS_SK_TLS_SAN_MAX];
    bool cut = true;

    espos_sk_tls_san_builder_t b = { 0 };
    san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, name, 63);
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_FALSE(cut);
    TEST_ASSERT_EQUAL(63, strlen(out));

    /* Kept as its first 63 bytes, a longer name would equal any other name
     * sharing them. */
    b = (espos_sk_tls_san_builder_t) { 0 };
    san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, name, 64);
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_TRUE(cut);
    TEST_ASSERT_EQUAL('!', out[0]);

    /* "boat.local\0.other" read as a C string would be "boat.local". */
    b = (espos_sk_tls_san_builder_t) { 0 };
    san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, "boat.local\0.other", 17);
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_TRUE(cut);
    TEST_ASSERT_FALSE(espos_sk_tls_san_equal(out, "boat.local"));
}

TEST_CASE("only host names count against the entry limit", "[sk_tls]")
{
    char out[ESPOS_SK_TLS_SAN_MAX];
    bool cut = true;
    char name[8];

    espos_sk_tls_san_builder_t b = { 0 };
    for (int i = 0; i < ESPOS_SK_TLS_SAN_ENTRY_MAX; i++) {
        int len = snprintf(name, sizeof(name), "h%d.lan", i);
        san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, name, (size_t)len);
    }
    san_add_str(&b, 1, "ops@boat.lan", 12); /* an e-mail address after eight names */
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_FALSE(cut);

    /* A ninth host name does not fit. */
    san_add_str(&b, ESPOS_SK_TLS_SAN_TAG_DNS, "h8.lan", 6);
    espos_sk_tls_san_finish(&b, out, sizeof(out), &cut);
    TEST_ASSERT_TRUE(cut);
    TEST_ASSERT_EQUAL('!', out[0]);
}
