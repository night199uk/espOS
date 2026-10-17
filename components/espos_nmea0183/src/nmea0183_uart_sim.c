/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The linux target has no UART: everything above the driver is host-tested
 * through espos_nmea0183.h directly.
 */
#include <string.h>

#include "espos_nmea0183_uart.h"

esp_err_t espos_nmea0183_uart_open(const espos_nmea0183_uart_cfg_t *cfg, espos_nmea0183_uart_t *out)
{
    (void)cfg;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

void espos_nmea0183_uart_close(espos_nmea0183_uart_t h)
{
    (void)h;
}

int32_t espos_nmea0183_uart_add_listener(espos_nmea0183_uart_t h, espos_nmea0183_sentence_fn fn, void *arg)
{
    (void)h;
    (void)fn;
    (void)arg;
    return 0;
}

void espos_nmea0183_uart_remove_listener(espos_nmea0183_uart_t h, int32_t id)
{
    (void)h;
    (void)id;
}

esp_err_t espos_nmea0183_uart_write(espos_nmea0183_uart_t h, const char *line)
{
    (void)h;
    (void)line;
    return ESP_ERR_NOT_SUPPORTED;
}

void espos_nmea0183_uart_get_stats(espos_nmea0183_uart_t h, espos_nmea0183_uart_stats_t *out)
{
    (void)h;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
