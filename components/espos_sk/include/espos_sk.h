/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sk — SignalK server discovery and access-token management (M3);
 * WebSocket delta output and meta reconciliation follow in M4.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "espos_sk_token_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A boat network can carry more SignalK servers than you would guess: a
 * plotter, a spare Pi, a laptop running one for development, plus every
 * neighbouring vessel in the marina. Measured on one: 9 advertisements.
 * When the list is smaller than what is advertised, which entries survive
 * depends on mDNS answer order, which is not stable — so the server the
 * device is actually paired with can silently drop out. */
#define ESPOS_SK_MAX_SERVERS 12

typedef struct {
    char host[ESPOS_SK_HOST_MAX];   /* IPv4 dotted or hostname */
    uint16_t port;
    char self[ESPOS_SK_SELF_MAX];
    char name[48];                  /* mDNS instance name */
    char roles[32];
    char swname[24];
    char swvers[16];
    /* The server advertised itself as _signalk-https._tcp rather than
     * _signalk-http._tcp. signalk-server publishes one or the other depending
     * on its `ssl` setting (src/interfaces/rest.js), so this is the server
     * telling us its scheme -- which is what sk.scheme = auto reads. */
    bool tls;
    uint32_t seen_ms;
} espos_sk_discovered_t;

/** Start the SignalK task: loads config + persistent state, runs discovery
 * and the token machine, registers the /api/v1/sk endpoints. Needs espos_config,
 * espos_httpd and (on device) espos_wifi to be started. */
esp_err_t espos_sk_start(void);
esp_err_t espos_sk_stop(void);

/** Status document of docs/rest-api.md (malloc'ed JSON). */
esp_err_t espos_sk_status_json(char **out_json);
/** Discovered servers as JSON array document {"servers":[...]} (malloc'ed). */
esp_err_t espos_sk_servers_json(char **out_json);

/* Commands (thread-safe, queued to the SK task). */
esp_err_t espos_sk_discover_now(void);
esp_err_t espos_sk_request_now(void);            /* re-request access (from denied/error) */
esp_err_t espos_sk_set_token(const char *token); /* manual token paste */
esp_err_t espos_sk_forget_token(void);           /* drop token + pending, request again */
/** Report that some SK call was rejected with 401/403 (M4 uses this). */
void espos_sk_report_unauthorized(void);
/**
 * Report that the server's TLS certificate was refused: the token machine
 * enters `cert_error`, keeps the token (the credential is fine, the transport
 * is not) and retries on a flat 60 s. `reason` is the sentence an operator
 * reads; NULL for a generic one. Thread-safe, queued to the SK task.
 */
void espos_sk_report_cert_error(const char *reason);

/* ------------------------------------------------------------ deltas */

/** Publish a value for a SignalK path (vessels.self). Values are batched
 * (sk.batch_ms), buffered while offline and streamed over the WebSocket.
 * Thread-safe; never blocks. */
esp_err_t espos_sk_publish_number(const char *path, double value);
esp_err_t espos_sk_publish_string(const char *path, const char *value);
esp_err_t espos_sk_publish_bool(const char *path, bool value);
/** value_json is a complete JSON value (object, array, null, …). */
esp_err_t espos_sk_publish_json(const char *path, const char *value_json);

/**
 * Publish for another vessel: a dinghy logger relaying its own sounder to the
 * mothership's server, a sensor reporting for a boat it is not on.
 * `context` is a full SignalK context, usually espos_sk_vessel_context() or
 * "vessels.urn:mrn:signalk:mmsi:<mmsi>"; NULL is refused (ESP_ERR_INVALID_ARG)
 * rather than read as vessels.self. Batched, buffered and timestamped like the
 * calls above; a batch window sends one delta per context.
 *
 * For live data only. The server takes the newest delta it receives as the
 * vessel's current state whatever its timestamp (signalk-server
 * src/deltacache.ts), so data recorded hours ago belongs in a batch upload,
 * not here. A write-permission token may publish any context unless the
 * server's ACLs say otherwise.
 */
esp_err_t espos_sk_publish_number_ctx(const char *context, const char *path, double value);
esp_err_t espos_sk_publish_string_ctx(const char *context, const char *path, const char *value);
esp_err_t espos_sk_publish_bool_ctx(const char *context, const char *path, bool value);
esp_err_t espos_sk_publish_json_ctx(const char *context, const char *path, const char *value_json);

/**
 * This device's own vessel, "vessels.urn:mrn:signalk:uuid:<uuid>": for a
 * device that is a vessel of its own (a dinghy, a tender) rather than a part
 * of the boat whose server it talks to. The UUID is generated on the first
 * espos_sk_start() and kept in NVS, so it survives reboots and OTA; erasing
 * NVS makes a new vessel. NULL before espos_sk_start() or when NVS cannot hold
 * it. Also in the status document as `vessel_context`.
 */
const char *espos_sk_vessel_context(void);

/**
 * Raise or clear a SignalK notification under notifications.espos.LABEL.KEY (the device label and the condition key).
 *
 * For conditions the device knows about and an operator would want to see:
 * memory pressure, an overheating chip, a service the firmware depends on
 * having gone away. These otherwise surface as a device that has quietly
 * stopped doing its job, which is indistinguishable from a hardware fault
 * from the outside and is the expensive kind of problem to diagnose.
 *
 * Notifications are level-triggered and idempotent: raising the same state and
 * message twice sends one delta, so a caller may poll and re-raise freely.
 * Passing ESPOS_SK_ALERT_NORMAL clears the condition.
 *
 * `key` is a short stable identifier ("lowMemory", "wakeService"), not a
 * sentence -- it becomes part of the path, and the path is what a rule or a
 * dashboard keys on. `message` is the human-readable half and may change
 * without re-notifying.
 *
 * Thread-safe; never blocks. Buffered like any other delta while offline.
 */
typedef enum {
    ESPOS_SK_ALERT_NORMAL = 0,  /* condition cleared */
    ESPOS_SK_ALERT_WARN,
    ESPOS_SK_ALERT_ALARM,
} espos_sk_alert_t;

esp_err_t espos_sk_notify(const char *key, espos_sk_alert_t state, const char *message);

/**
 * Declare metadata for a NON-standard path (never for spec paths — the
 * server knows those). meta_json is the full meta object, e.g.
 * {"units":"Hz","description":"…"}. period_ms > 0 adds "timeout" (in
 * seconds, 2.5× the period) as the one field the device really owns.
 * Reconciled on every (re)connect: GET the server's meta, PUT only if it
 * is empty — server-side edits win. Up to ESPOS_SK_MAX_META entries.
 */
/* The compile-time cap. CONFIG_ESPOS_SK_MAX_META tunes the table the
 * implementation actually allocates; this is the number the API promises and
 * stays a literal, because a public header that reads a CONFIG_ token gives
 * two firmwares built from one header different ABIs. */
#define ESPOS_SK_MAX_META 32
esp_err_t espos_sk_declare_meta(const char *path, const char *meta_json, uint32_t period_ms);

/* ------------------------------------------------------- inbound (M7) */

#include "espos_sk_parse.h"

/**
 * Subscribe to values (and meta) of vessels.self paths. `pattern` is an
 * exact path or a family: "notifications.*", "environment.*", "*".
 * period_ms is the server-side rate hint (0 = 1000). cb runs on the stream
 * task with strings valid only during the call — copy, do not block, do
 * not call espos_sk_* that could wait on the stream. Meta arrives as items
 * with meta_json set (stream opened with sendMeta=all). Subscriptions
 * survive reconnects and are (re)sent after every hello. Returns a
 * handle > 0, or <0 (-ESP_ERR_NO_MEM style) when the table is full.
 */
typedef void (*espos_sk_sub_cb_t)(const espos_sk_update_t *u, void *arg);
#define ESPOS_SK_MAX_SUBS 48
int espos_sk_subscribe(const char *pattern, uint32_t period_ms, espos_sk_sub_cb_t cb, void *arg);
esp_err_t espos_sk_unsubscribe(int handle);

/**
 * PUT a value (JSON text) to a vessels.self path over the stream. cb (may
 * be NULL) gets the server's response — state COMPLETED/FAILED with
 * statusCode — or state "TIMEOUT" after 10 s. Queued if the stream is
 * momentarily busy; ESP_ERR_INVALID_STATE when not connected, ESP_ERR_NO_MEM
 * when 8 requests are already in flight.
 */
typedef void (*espos_sk_put_cb_t)(const char *request_id, const char *state, int status_code, const char *message, void *arg);
esp_err_t espos_sk_put(const char *path, const char *value_json, espos_sk_put_cb_t cb, void *arg);

/** Send an arbitrary text frame on the stream (e.g. an inbound delta the
 * server should ingest as-is). ESP_ERR_INVALID_STATE when not connected. */
esp_err_t espos_sk_send_raw(const char *json);

/* -------------------------------------------------- inbound PUT (control) */

/**
 * Handle a PUT the SERVER sends to this device: how a phone operates a
 * switch. The counterpart of espos_sk_put(), which goes the other way.
 *
 * The server only routes a PUT to a device it has seen publish that path,
 * so a controllable path must be published at least once (any
 * espos_sk_publish_*) before a request can arrive -- signalk-server keys
 * its route on the (path, $source) pairs it has observed on this
 * connection.
 *
 * `value_json` is the requested value as JSON text ("true", "0.5",
 * "\"auto\"", "null"). Return:
 *   ESP_OK              applied -- answered COMPLETED 200,
 *   ESP_ERR_INVALID_ARG the value made no sense -- COMPLETED 400,
 *   anything else       COMPLETED 502.
 * Answer later instead by returning ESPOS_SK_PUT_PENDING and calling
 * espos_sk_put_respond() when the work is done; the server waits 60 s.
 *
 * cb runs on the stream task: copy what you need, do not block, and do not
 * call back into espos_sk_* calls that wait on the stream. Publishing the
 * new value is the normal thing to do and is safe (it never blocks).
 *
 * A path with no handler is answered COMPLETED 405, which is what
 * signalk-server itself replies for an unhandled path (src/put.ts) and what
 * a client expects.
 */
typedef esp_err_t (*espos_sk_put_handler_t)(const char *path, const char *value_json, void *arg);

/** Returned by a handler that will answer later via espos_sk_put_respond(). */
#define ESPOS_SK_PUT_PENDING 1

#define ESPOS_SK_MAX_PUT_HANDLERS 16
esp_err_t espos_sk_put_handler_register(const char *path, espos_sk_put_handler_t cb, void *arg);
esp_err_t espos_sk_put_handler_unregister(const char *path);

/**
 * Answer a PUT request. Only needed after a handler returned
 * ESPOS_SK_PUT_PENDING -- every other outcome is answered automatically.
 *
 * `state` is "COMPLETED" or "PENDING": signalk-server accepts nothing else
 * on this path (src/interfaces/ws.ts, isWsRequestReply) and silently drops
 * a reply carrying anything else, which reads as a request that timed out
 * 60 s later. A failure is COMPLETED with a 4xx/5xx statusCode, not a
 * "FAILED" state.
 *
 * Thread-safe; may be called from any task. ESP_ERR_INVALID_STATE when the
 * stream is not connected.
 */
esp_err_t espos_sk_put_respond(const char *request_id, const char *state, int status_code, const char *message);

/**
 * Send everything buffered and wait until the stream has drained, up to
 * timeout_ms. The prerequisite for deep sleep: a delta published a
 * millisecond before esp_deep_sleep_start() is otherwise still sitting in
 * the batch buffer when the radio goes down.
 *
 * Returns ESP_OK when nothing is left to send, ESP_ERR_TIMEOUT when the
 * deadline passed with data still pending, ESP_ERR_INVALID_STATE when the
 * stream is not connected (nothing can drain, so the caller should not
 * wait). Blocks the calling task; never call it from the stream task or a
 * subscription callback.
 *
 * A message being written counts as pending until the write returns, so
 * ESP_OK means every message was handed to the socket. It does not mean the
 * server acknowledged it: on a live link the TCP stack sends within
 * milliseconds, and a caller about to cut the radio should allow that.
 */
esp_err_t espos_sk_flush(uint32_t timeout_ms);

typedef struct {
    bool enabled;
    bool connected;
    uint32_t connected_s;
    uint32_t reconnects;      /* successful connections so far */
    uint32_t sent;            /* messages sent */
    uint32_t send_errors;
    uint32_t next_retry_s;    /* while disconnected */
    char last_error[64];
    size_t pending, buffered, buffered_bytes;
    uint32_t dropped;
    size_t meta_declared, meta_reconciled;
    uint32_t received;        /* value/meta items delivered to subscribers */
    uint32_t frames;          /* text frames read */
    size_t subs;              /* active subscriptions */
    size_t puts_pending;
    uint32_t puts_sent, puts_failed;
    uint32_t puts_in;         /* inbound PUT items received from the server */
    uint32_t puts_rejected;   /* of those, answered 405 (no handler) or 502 */
} espos_sk_ws_status_t;
esp_err_t espos_sk_ws_get_status(espos_sk_ws_status_t *out);

/** Copy the current usable token ("" if none). Thread-safe. */
esp_err_t espos_sk_get_token(char *buf, size_t size);
/** Copy the current server (host/port/self); ESP_ERR_NOT_FOUND if none. */
esp_err_t espos_sk_get_server(espos_sk_server_t *out);
const char *espos_sk_client_id(void);

/**
 * Name the device in the server's access-request list. The default
 * description is "<name> <hostname>" when sk.description is empty; without a
 * name it is "espOS <hostname>", which tells an operator approving five
 * requests nothing. espos_start() passes the application name. Takes effect
 * on the next configuration load (before espos_sk_start(), or a config
 * change). Copies at most 32 characters; ESP_ERR_INVALID_ARG on NULL.
 */
esp_err_t espos_sk_set_app_name(const char *name);

#ifdef __cplusplus
}
#endif
