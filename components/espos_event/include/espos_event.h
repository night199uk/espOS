/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_event — the espOS event bus: one esp_event base, ESPOS_EVENT, on
 * the default event loop. Components post the milestones of a device's
 * life here (config up, network up, server found, token approved, update
 * available) so an application — or another component — can react to them
 * without polling status snapshots and without depending on the component
 * that produced them.
 *
 * Threading contract: handlers run on the default event loop task, the one
 * esp_event_loop_create_default() made and WIFI_EVENT / IP_EVENT share.
 * Copy `event_data` out and return; it is valid for the duration of the
 * call only. Never block in a handler and never wait for another task from
 * one — the WiFi driver's own events queue behind it.
 *
 * This header includes esp_event.h, the one exception to the rule that a
 * public espOS header pulls in no IDF header but esp_err.h: the type on
 * offer here IS the IDF event loop. The base, the handler signature and the
 * subscribe call are esp_event's, and wrapping them in look-alike copies
 * would only make the same loop harder to reach from an application that
 * already handles WIFI_EVENT on it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(ESPOS_EVENT);

/* Event ids. Values are part of the ABI: append, never renumber. */
typedef enum {
    ESPOS_EVENT_CONFIG_READY = 1,           /* espos_config is up (espos_init); data: none */
    ESPOS_EVENT_HTTPD_STARTED = 2,          /* REST server listening; data: none */
    ESPOS_EVENT_NETWORK_UP = 3,             /* station got an IP; data: espos_event_network_t */
    ESPOS_EVENT_NETWORK_DOWN = 4,           /* station link lost; one per NETWORK_UP; data: none */
    ESPOS_EVENT_MDNS_READY = 5,             /* mDNS responder up; data: none */
    ESPOS_EVENT_SK_SERVER_SELECTED = 6,     /* a SignalK server was chosen; data: espos_event_sk_server_t */
    ESPOS_EVENT_SK_TOKEN_APPROVED = 7,      /* access token verified; data: none */
    ESPOS_EVENT_SK_STREAM_CONNECTED = 8,    /* delta stream open; data: none */
    ESPOS_EVENT_SK_STREAM_DISCONNECTED = 9, /* delta stream closed; data: none */
    ESPOS_EVENT_OTA_AVAILABLE = 10,         /* manifest names a newer build; data: espos_event_ota_t */
    ESPOS_EVENT_TIME_SYNCED = 11,           /* a source set the wall clock; data: espos_event_time_t */
    /* The setup access point came up / went away. Anything else that shares
     * the radio should get out of the way in between: on a co-processor part
     * (the ESP32-P4's C6) WiFi and BLE are ONE radio, and a BLE scan at the
     * default 160/320 ms duty cycle takes half of it -- enough to make
     * joining the portal take minutes, or fail. data: none */
    ESPOS_EVENT_PORTAL_UP = 12,
    ESPOS_EVENT_PORTAL_DOWN = 13,
    /* A firmware image is downloading -- this chip's (espos_ota) or a radio
     * co-processor's -- and has finished, successfully or not. One FINISHED
     * per STARTED. Posted beside espos_ota_quiesce_hook()/resume_hook(), for
     * work that wants out of the way but is not the hook's one owner: a BLE
     * scan competes with the download for airtime and internal RAM. A
     * successful install reboots shortly after FINISHED. data: none */
    ESPOS_EVENT_UPDATE_STARTED = 14,
    ESPOS_EVENT_UPDATE_FINISHED = 15,
    ESPOS_EVENT_MAX = 16,
} espos_event_id_t;

/* ESPOS_EVENT_NETWORK_UP */
typedef struct {
    char ip[16];       /* dotted IPv4 */
    char hostname[33]; /* what the device answers to as <hostname>.local */
} espos_event_network_t;

/* ESPOS_EVENT_SK_SERVER_SELECTED */
typedef struct {
    char host[64]; /* IPv4 dotted or hostname, as espos_sk uses it */
    uint16_t port;
} espos_event_sk_server_t;

/* ESPOS_EVENT_OTA_AVAILABLE */
typedef struct {
    char version[32]; /* the manifest's version string */
} espos_event_ota_t;

/* ESPOS_EVENT_TIME_SYNCED. `source` is an espos_time_src_t, carried as a
 * plain integer so this header stays a leaf — espos_event depends on nothing
 * of espOS, which is what lets any component post to it. A subscriber that
 * cares which source it was casts it; most only care that there now is one. */
typedef struct {
    uint8_t source;  /* espos_time_src_t: 1 rtc, 2 sk, 3 manual, 4 sntp */
    int64_t unix_ms; /* the instant the clock was set to */
} espos_event_time_t;

/**
 * Post an ESPOS_EVENT to the default loop, creating the loop if nobody has
 * yet (espos_wifi or an application may already have; that is fine). `data`
 * is copied, `size` bytes of it; NULL/0 for events without data.
 *
 * Thread-safe. Never blocks for long: waits at most a few ticks for queue
 * space, none at all when called from the loop's own task (from inside a
 * handler). ESP_ERR_TIMEOUT when the queue is full — the event is dropped,
 * which callers treat as acceptable for a notification.
 */
esp_err_t espos_event_post(int32_t id, const void *data, size_t size);

/**
 * Run `handler` on the default loop task for ESPOS_EVENT `id`
 * (ESP_EVENT_ANY_ID for all of them). `arg` is handed back untouched. The
 * same (handler, arg) pair registered twice is called once.
 */
esp_err_t espos_event_subscribe(int32_t id, esp_event_handler_t handler, void *arg);
esp_err_t espos_event_unsubscribe(int32_t id, esp_event_handler_t handler);

#ifdef __cplusplus
}
#endif
