/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * SignalK access-token state machine — pure C. Drives the access-request
 * flow of the SignalK security API:
 *
 *   IDLE ──POST /signalk/v1/access/requests──▶ REQUESTED (poll href, backoff 5 s → 60 s)
 *          ├─ 404 security disabled ─────────▶ OPEN (no token needed)
 *          └─ 403 device requests disabled ──▶ DENIED
 *   REQUESTED ─ COMPLETED/APPROVED ▶ store token ▶ VERIFYING ▶ APPROVED
 *             ─ COMPLETED/DENIED   ▶ DENIED (no auto retry)
 *             ─ href gone (404/500 not found) ▶ IDLE (re-request)
 *   APPROVED  ─ periodic GET /signalk/v1/api/self with the token
 *             ─ 401/403 ▶ token invalidated ▶ IDLE
 *   VERIFYING ─ 200 ▶ APPROVED (self URN learned)   ─ 401/403 ▶ IDLE
 *   any leg   ─ the server's certificate is not the trusted one ▶ CERT_ERROR
 *               (token kept, flat 60 s retry, stream held down)
 *
 * Two rules that are not obvious from the diagram:
 *   - over plaintext a single 401 does NOT clear the token (anything on the
 *     path can answer one); two consecutive unauthorised answers do. Over TLS
 *     one is conclusive. See plain_unauth_streak below.
 *   - a TLS server skips the VERIFYING leg after an approval: the WebSocket
 *     upgrade carries the same token and rejects it just as clearly, so the
 *     leg would only buy a second handshake per reconnect.
 *
 * The machine never touches storage or HTTP itself: it asks the port to
 * do things (start a request, poll, verify, save/clear) and is fed the
 * outcomes as events. It runs unchanged on the host under test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_SK_HOST_MAX  64
#define ESPOS_SK_SELF_MAX  128
#define ESPOS_SK_HREF_MAX  128
#define ESPOS_SK_TOKEN_MAX 1024
#define ESPOS_SK_MSG_MAX   96

typedef enum {
    ESPOS_SK_TOK_NO_SERVER = 0, /* nothing to talk to yet */
    ESPOS_SK_TOK_IDLE,          /* server known; a request will be sent */
    ESPOS_SK_TOK_REQUESTED,     /* pending approval; polling href */
    ESPOS_SK_TOK_VERIFYING,     /* have a token; confirming it works */
    ESPOS_SK_TOK_APPROVED,      /* token works */
    ESPOS_SK_TOK_DENIED,        /* admin denied (or device requests disabled); waits for user */
    ESPOS_SK_TOK_OPEN,          /* server security disabled: no token needed */
    ESPOS_SK_TOK_ERROR,         /* transient trouble (server unreachable); retrying with backoff */
    /* The server's certificate is not the one this device trusts. Not an
     * error to back off from: an exponential retry would have the device
     * checking once an hour by the time somebody looks at it, and the fix
     * (renew the certificate, or trust the new one) lands from outside and
     * should be noticed within a minute. So a flat 60 s, and the token is
     * kept — it is the transport that is wrong, not the credential. The
     * stream stays down while this holds (espos_sk_stream_allowed() is false):
     * falling back to plaintext would send the token to whoever answered. */
    ESPOS_SK_TOK_CERT_ERROR,
} espos_sk_tok_state_t;

typedef struct {
    char host[ESPOS_SK_HOST_MAX];
    uint16_t port;
    /* Scheme for this server: https/wss when true, http/ws when false. Per
     * server and not global, which is what makes sk.scheme = auto possible: a
     * discovered server's scheme comes from the mDNS service type it answered
     * on, a manual host's from one redirect probe, and the value is cached
     * with the server rather than re-decided per call. Effective only in
     * builds with CONFIG_ESPOS_SK_TLS; see docs/signalk.md. */
    bool tls;
    char self[ESPOS_SK_SELF_MAX];   /* vessel self URN if known (mDNS TXT / learned), else "" */
} espos_sk_server_t;

typedef struct {
    char client_id[40];             /* persistent UUID */
    char description[80];
    char permissions[12];           /* readonly | readwrite | admin */
    uint32_t check_interval_ms;     /* token re-verification while APPROVED */
} espos_sk_tok_cfg_t;

/* Persistent state handed in at start and written back through the port. */
typedef struct {
    char token[ESPOS_SK_TOKEN_MAX];
    char token_self[ESPOS_SK_SELF_MAX];   /* the server the token belongs to */
    /* ...and the address it was issued at. self alone does not identify a
     * server: a second install configured with the same MMSI (a test VM, a
     * spare Pi) advertises the same URN, and a token tried there is refused
     * and dropped. "" for a token stored before this field existed. */
    char token_host[ESPOS_SK_HOST_MAX];
    uint16_t token_port;
    char pending_href[ESPOS_SK_HREF_MAX];
    char pending_host[ESPOS_SK_HOST_MAX];
    uint16_t pending_port;
    char pending_self[ESPOS_SK_SELF_MAX];
} espos_sk_tok_store_t;

typedef enum {
    ESPOS_SK_EV_START,            /* arg: NULL */
    ESPOS_SK_EV_STOP,
    ESPOS_SK_EV_SERVER,           /* arg: const espos_sk_server_t* (NULL = no server) */
    ESPOS_SK_EV_REQUEST_RESULT,   /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_POLL_RESULT,      /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_VERIFY_RESULT,    /* arg: const espos_sk_http_result_t* */
    ESPOS_SK_EV_TIMER,
    ESPOS_SK_EV_MANUAL_TOKEN,     /* arg: const char* token */
    ESPOS_SK_EV_RETRY,            /* user asks to request again (from DENIED/ERROR) */
    ESPOS_SK_EV_UNAUTHORIZED,     /* some other SK call got 401/403 with our token */
    ESPOS_SK_EV_CONFIG,           /* arg: const espos_sk_tok_cfg_t* */
    /* The TLS layer refused the server. arg: const char* reason (may be NULL).
     * Distinct from EV_UNAUTHORIZED because the token is fine and must be
     * kept: throwing it away over a certificate change would mean a fresh
     * approval in the server UI every time the certificate is renewed. */
    ESPOS_SK_EV_CERT_ERROR,
} espos_sk_tok_event_t;

typedef struct {
    int http_status;              /* 0 = transport failure (unreachable) */
    /* request / poll */
    char state[12];               /* "PENDING" | "COMPLETED" | "" */
    char permission[12];          /* "APPROVED" | "DENIED" | "" */
    char href[ESPOS_SK_HREF_MAX];
    char token[ESPOS_SK_TOKEN_MAX];
    char message[ESPOS_SK_MSG_MAX];
    /* verify */
    char self[ESPOS_SK_SELF_MAX];
    /* The transport, not the reply: the connection never got far enough to
     * have an HTTP status because the certificate was refused. http_status is
     * 0 in that case, which on its own reads as "server unreachable" and would
     * back off for five minutes over something an operator can fix in ten
     * seconds. */
    bool cert_error;
    char cert_reason[64];
} espos_sk_http_result_t;

/* Port: everything the machine needs from the outside world. Actions are
 * asynchronous — the port answers with the *_RESULT events. */
typedef struct {
    void (*http_request)(void *ctx, const espos_sk_server_t *srv, const espos_sk_tok_cfg_t *cfg);
    void (*http_poll)(void *ctx, const espos_sk_server_t *srv, const char *href);
    void (*http_verify)(void *ctx, const espos_sk_server_t *srv, const char *token);
    void (*store_save)(void *ctx, const espos_sk_tok_store_t *st);
    void (*arm_timer)(void *ctx, uint32_t ms);
    void (*cancel_timer)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);
    void (*status_changed)(void *ctx);
} espos_sk_tok_port_t;

typedef struct {
    espos_sk_tok_state_t state;
    espos_sk_server_t server;
    bool has_server;
    bool has_token;
    char pending_href[ESPOS_SK_HREF_MAX];
    uint32_t next_action_ms;      /* when the next poll/retry/check is due (port clock) */
    uint32_t poll_interval_ms;    /* current poll backoff */
    uint32_t requested_since_ms;
    uint32_t approved_since_ms;
    uint32_t last_check_ms;
    int last_http_status;
    char last_error[ESPOS_SK_MSG_MAX];
    uint32_t request_count, approve_count, deny_count, unauthorized_count, cert_error_count;
    bool busy;                    /* an HTTP action is in flight */
} espos_sk_tok_status_t;

typedef struct espos_sk_tok_sm {
    const espos_sk_tok_port_t *port;
    void *ctx;
    espos_sk_tok_cfg_t cfg;
    espos_sk_tok_store_t store;
    espos_sk_tok_status_t st;
    bool started;
    bool reeval;                  /* server/token changed while an action was in flight */
    uint32_t error_backoff_ms;
    /* Separate from error_backoff_ms on purpose: a duplicate pending request is
     * a wait for a person, not a fault to retry out of, so it has its own
     * ladder and its own (shorter) ceiling. Sharing one counter would let an
     * unreachable server stretch the approval wait, or vice versa. */
    uint32_t dup_backoff_ms;
    uint32_t timer_due_ms;
    /* Consecutive unauthorised answers over a PLAINTEXT connection. On http a
     * single 401 is not proof the token died: anything on the path can answer
     * one — a captive portal, a proxy, a router's "you are not logged in"
     * page — and throwing the token away over it costs a trip to the server's
     * admin UI to approve the device again. Over TLS nothing can inject an
     * answer, so one 401 is conclusive there and this counter is not used.
     * Reset by any 200. (SensESP's should_clear_token_on_status.) */
    uint32_t plain_unauth_streak;
} espos_sk_tok_sm_t;

void espos_sk_tok_init(espos_sk_tok_sm_t *sm, const espos_sk_tok_port_t *port, void *ctx,
                       const espos_sk_tok_cfg_t *cfg, const espos_sk_tok_store_t *store);
void espos_sk_tok_event(espos_sk_tok_sm_t *sm, espos_sk_tok_event_t ev, const void *arg);
const espos_sk_tok_status_t *espos_sk_tok_status(const espos_sk_tok_sm_t *sm);
/* Current token ("" if none/unusable). Valid to read on the machine's task. */
const char *espos_sk_tok_token(const espos_sk_tok_sm_t *sm);
const char *espos_sk_tok_state_str(espos_sk_tok_state_t s);

#ifdef __cplusplus
}
#endif
