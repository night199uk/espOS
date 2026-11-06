/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "unity.h"
#include "espos_sk_token_sm.h"
#include "espos_sk_priv.h"

/* main/CMakeLists.txt builds this project with CONFIG_ESPOS_SK_TOKEN_MAX=128.
 * Keep CAP in step with that definition: if they drift, this test fails. */
#define CAP 128

static struct {
    uint32_t now;
    bool timer_armed;
    int requests, polls, verifies, saves;
    char last_verify_token[ESPOS_SK_TOKEN_MAX];
    espos_sk_tok_store_t saved;
} F;

static void f_request(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg)
{
    (void)ctx;
    (void)srv;
    (void)cfg;
    F.requests++;
}
static void f_poll(void *ctx, const espos_sk_server_t *srv, const char *href)
{
    (void)ctx;
    (void)srv;
    (void)href;
    F.polls++;
}
static void f_verify(void *ctx, const espos_sk_server_t *srv, const char *tok)
{
    (void)ctx;
    (void)srv;
    F.verifies++;
    snprintf(F.last_verify_token, sizeof(F.last_verify_token), "%s", tok);
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
    (void)ms;
    F.timer_armed = true;
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
    return 0;
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
};

static espos_sk_tok_sm_t SM;
static const espos_sk_tok_cfg_t CFG = { .client_id = "11111111-2222-4333-8444-555555555555",
                                        .description = "espOS test",
                                        .permissions = "readwrite",
                                        .check_interval_ms = 60000 };
static const espos_sk_server_t SRV = { .host = "10.0.0.10", .port = 80, .self = "urn:mrn:signalk:uuid:aaaa" };

static void reset(const espos_sk_tok_store_t *store)
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    espos_sk_tok_init(&SM, &PORT, NULL, &CFG, store);
}

/* A token of len characters. */
static void make_token(char *out, size_t len)
{
    memset(out, 'a', len);
    out[len] = '\0';
}

/* Request, then answer the first poll with an approval carrying tok. */
static void approve_with(const char *tok)
{
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV);
    espos_sk_http_result_t req = { .http_status = 202, .state = "PENDING", .href = "/signalk/v1/requests/abc" };
    espos_sk_tok_event(&SM, ESPOS_SK_EV_REQUEST_RESULT, &req);
    F.now += 5000;
    espos_sk_tok_event(&SM, ESPOS_SK_EV_TIMER, NULL);
    TEST_ASSERT_EQUAL(1, F.polls);
    espos_sk_http_result_t poll = { .http_status = 200, .state = "COMPLETED", .permission = "APPROVED" };
    snprintf(poll.token, sizeof(poll.token), "%s", tok);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_POLL_RESULT, &poll);
}

TEST_CASE("token cap: the private cap follows the Kconfig", "[sk_cap]")
{
    TEST_ASSERT_EQUAL(CAP, ESPOS_SK_TOKEN_CAP);
}

TEST_CASE("token cap: an approved token that fits is stored and verified whole", "[sk_cap]")
{
    char tok[CAP];
    make_token(tok, CAP - 1);
    reset(NULL);
    approve_with(tok);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_VERIFYING, espos_sk_tok_status(&SM)->state);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL_STRING(tok, F.last_verify_token);
    TEST_ASSERT_EQUAL_STRING(tok, F.saved.token);
}

TEST_CASE("token cap: an approved token over the cap is refused, not clipped", "[sk_cap]")
{
    char tok[CAP + 1];
    make_token(tok, CAP);
    reset(NULL);
    approve_with(tok);
    const espos_sk_tok_status_t *st = espos_sk_tok_status(&SM);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_DENIED, st->state);
    TEST_ASSERT_NOT_NULL(strstr(st->last_error, "CONFIG_ESPOS_SK_TOKEN_MAX"));
    TEST_ASSERT_EQUAL(0, F.verifies);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token);
    TEST_ASSERT_EQUAL_STRING("", F.saved.pending_href);
    TEST_ASSERT_FALSE(st->has_token);
    TEST_ASSERT_EQUAL(1, st->deny_count);
    TEST_ASSERT_FALSE(F.timer_armed); /* no timed re-request for an admin to approve again */
    TEST_ASSERT_EQUAL_STRING("", espos_sk_tok_token(&SM));
    TEST_ASSERT_EQUAL(1, F.requests);
}

TEST_CASE("token cap: a stored token over the cap is dropped and access requested again", "[sk_cap]")
{
    espos_sk_tok_store_t store = { .token_self = "urn:mrn:signalk:uuid:aaaa", .token_host = "10.0.0.10", .token_port = 80 };
    make_token(store.token, CAP);
    reset(&store);
    TEST_ASSERT_FALSE(espos_sk_tok_status(&SM)->has_token);
    TEST_ASSERT_NOT_NULL(strstr(espos_sk_tok_status(&SM)->last_error, "CONFIG_ESPOS_SK_TOKEN_MAX"));
    TEST_ASSERT_EQUAL(1, F.saves); /* saved dropped: the next boot does not drop it again */
    TEST_ASSERT_EQUAL_STRING("", F.saved.token);
    TEST_ASSERT_EQUAL_STRING("", F.saved.token_host);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV);
    TEST_ASSERT_EQUAL(0, F.verifies);
    TEST_ASSERT_EQUAL(1, F.requests);
    TEST_ASSERT_EQUAL(ESPOS_SK_TOK_IDLE, espos_sk_tok_status(&SM)->state);
}

TEST_CASE("token cap: a stored token that fits is verified as before", "[sk_cap]")
{
    espos_sk_tok_store_t store = { .token_self = "urn:mrn:signalk:uuid:aaaa", .token_host = "10.0.0.10", .token_port = 80 };
    make_token(store.token, CAP - 1);
    reset(&store);
    TEST_ASSERT_EQUAL(0, F.saves);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_SERVER, &SRV);
    TEST_ASSERT_EQUAL(1, F.verifies);
    TEST_ASSERT_EQUAL_STRING(store.token, F.last_verify_token);
    TEST_ASSERT_EQUAL(0, F.requests);
}

TEST_CASE("token cap: a manual token over the cap is ignored", "[sk_cap]")
{
    char tok[CAP + 1];
    make_token(tok, CAP);
    reset(NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_START, NULL);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_MANUAL_TOKEN, tok);
    TEST_ASSERT_FALSE(espos_sk_tok_status(&SM)->has_token);
    TEST_ASSERT_EQUAL_STRING("", SM.store.token);
    make_token(tok, CAP - 1);
    espos_sk_tok_event(&SM, ESPOS_SK_EV_MANUAL_TOKEN, tok);
    TEST_ASSERT_EQUAL_STRING(tok, SM.store.token);
}
