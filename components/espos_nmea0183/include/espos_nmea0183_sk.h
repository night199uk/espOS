/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_nmea0183_sk — what a sentence means in Signal K.
 *
 * Two layers, like the rest of espOS: espos_nmea0183_sk_map() is pure C and
 * turns one sentence into the Signal K values it carries (host-tested), and
 * espos_nmea0183_sk_publish() sends those values on the stream, when espos_sk
 * is in the build. The paths follow signalk-server's own NMEA 0183 parser, so
 * a GPS on an espOS device and the same GPS on the server's serial port show
 * up identically.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "espos_nmea0183.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_NMEA0183_SK_MAX_UPDATES 8
#define ESPOS_NMEA0183_SK_JSON_MAX    64

typedef enum {
    ESPOS_NMEA0183_SK_NUMBER = 0,
    ESPOS_NMEA0183_SK_JSON = 1,   /* an object: navigation.position */
    ESPOS_NMEA0183_SK_STRING = 2, /* navigation.datetime, methodQuality */
    ESPOS_NMEA0183_SK_KIND_MAX = 3,
} espos_nmea0183_sk_kind_t;

typedef struct {
    const char *path; /* a constant, never freed */
    espos_nmea0183_sk_kind_t kind;
    double number;
    char text[ESPOS_NMEA0183_SK_JSON_MAX]; /* JSON or string, by kind */
} espos_nmea0183_sk_update_t;

/**
 * The Signal K values `s` carries, into `out` (up to `max`). Returns how many.
 *
 * Nothing is reported that the sentence does not say: no position without a
 * fix, no course from a receiver whose mode says it has none, no depth from
 * an empty field, no navigation.datetime before 2020 (a battery-backed clock
 * after a GPS week rollover). Unknown sentences yield 0.
 */
size_t espos_nmea0183_sk_map(const espos_nmea0183_sentence_t *s, espos_nmea0183_sk_update_t *out, size_t max);

/**
 * The instant `s` says it is, in unix milliseconds, or 0. Only an RMC with a
 * valid fix counts, and only from 2020 on: a receiver without a fix reports
 * its battery-backed clock, which after a GPS week rollover is years out.
 */
int64_t espos_nmea0183_sk_clock_ms(const espos_nmea0183_sentence_t *s);

/** How far GPS time and the clock may drift apart before the clock is set
 * again. A fix every second would otherwise step the clock every second, by
 * the reception jitter, and timestamps would stop being monotonic. */
#define ESPOS_NMEA0183_SK_CLOCK_SLACK_MS 2000

/**
 * Whether GPS time `gps_ms` (from espos_nmea0183_sk_clock_ms) should set a
 * clock that reads `now_ms`: when it is not synced, or has drifted more than
 * ESPOS_NMEA0183_SK_CLOCK_SLACK_MS. Never when `gps_ms` is 0.
 */
bool espos_nmea0183_sk_clock_due(int64_t gps_ms, int64_t now_ms, bool synced);

/**
 * Map `s` and publish every value on the Signal K stream. With `set_clock`,
 * an RMC with a valid fix also sets espOS's clock (espos_time_set, source
 * "manual") so deltas carry GPS time on a boat with no internet: once, then
 * again only after a drift (espos_nmea0183_sk_clock_due), and never over a
 * clock SNTP set. ESP_ERR_NOT_SUPPORTED when espos_sk is not in the build.
 */
esp_err_t espos_nmea0183_sk_publish(const espos_nmea0183_sentence_t *s, bool set_clock);

#ifdef __cplusplus
}
#endif
