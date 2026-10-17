/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_nmea0183_uart — NMEA 0183 sentences off a UART.
 *
 * One task per port reads bytes, assembles lines, checks and splits them, and
 * hands every good sentence to the listeners, so one GPS can feed a Signal K
 * publisher, a logger on a card and a TCP bridge at once.
 *
 * Wiring. NMEA 0183 is RS-422: a differential pair, A and B. Into an ESP32
 * pin it goes through a receiver (MAX3485, SN65HVD3082, the RS-485
 * transceiver many boards carry) — never straight in, the line swings beyond
 * the chip's 3.3 V. An older single-ended talker (NMEA 0183 1.x, or a "TTL"
 * GPS module behind an RS-232 level shifter) idles at the opposite level from
 * a UART; `invert_rx` flips the pin in the UART itself rather than adding an
 * inverter. A 3.3 V GPS module (u-blox NEO, ATGM336H) goes straight onto the
 * pin, not inverted, usually at 9600 baud.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espos_nmea0183.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_NMEA0183_UART_MAX_LISTENERS 4

typedef struct espos_nmea0183_uart *espos_nmea0183_uart_t;

typedef struct {
    int32_t uart_num;          /* 1 or 2; UART0 is usually the console */
    int32_t rx_gpio;
    int32_t tx_gpio;           /* -1: listen only, the common case */
    uint32_t baud;         /* 0 = 4800, the standard rate; GPS modules: 9600; AIS and "high speed": 38400 */
    bool invert_rx;        /* a single-ended talker wired without a receiver chip */
    bool require_checksum; /* refuse sentences without "*hh" (most talkers send one) */
    uint32_t task_stack;   /* 0 = 6144 bytes, enough for espos_nmea0183_sk_publish(); listeners run on this stack */
    int32_t task_prio;         /* 0 = 5 */
} espos_nmea0183_uart_cfg_t;

/**
 * Called for every sentence that parsed. `raw` is the line as received,
 * without CR LF (what a bridge forwards); `s` its fields. Both are valid only
 * until the callback returns. Runs on the port's task: copy what you need
 * and return; never call another espos_nmea0183_uart_* function from here.
 */
typedef void (*espos_nmea0183_sentence_fn)(const char *raw, const espos_nmea0183_sentence_t *s, void *arg);

typedef struct {
    uint32_t lines;        /* complete lines received */
    uint32_t sentences;    /* lines that parsed and went to the listeners */
    uint32_t bad_checksum; /* a sentence whose checksum did not match: noise on the wire */
    uint32_t rejected;     /* any other parse error */
    uint32_t overflows;    /* lines dropped for being too long */
    uint32_t resyncs;      /* lines cut short by a new start character */
    uint32_t garbled;      /* lines dropped for a byte that is not printable ASCII */
} espos_nmea0183_uart_stats_t;

/** Install the UART driver and start reading. ESP_ERR_NOT_SUPPORTED on the
 * linux target. */
esp_err_t espos_nmea0183_uart_open(const espos_nmea0183_uart_cfg_t *cfg, espos_nmea0183_uart_t *out);

/** Stop the task and remove the driver. Listeners are not called after it
 * returns. */
void espos_nmea0183_uart_close(espos_nmea0183_uart_t h);

/** Add a listener; returns its id (> 0), or 0 when all slots are taken. */
int32_t espos_nmea0183_uart_add_listener(espos_nmea0183_uart_t h, espos_nmea0183_sentence_fn fn, void *arg);

/** Remove a listener. When it returns, the listener is not running and will
 * not run again, so `arg` may be freed. */
void espos_nmea0183_uart_remove_listener(espos_nmea0183_uart_t h, int32_t id);

/**
 * Send one sentence (needs a tx_gpio). `line` is "$IIXDR,..." with or without
 * its checksum and CR LF; both are added when missing.
 */
esp_err_t espos_nmea0183_uart_write(espos_nmea0183_uart_t h, const char *line);

void espos_nmea0183_uart_get_stats(espos_nmea0183_uart_t h, espos_nmea0183_uart_stats_t *out);

#ifdef __cplusplus
}
#endif
