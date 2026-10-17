/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * A GPS on a UART, published to Signal K: position, course and speed over
 * ground, fix quality and satellites, and the clock set from the fix. The
 * same code reads a depth sounder or a compass that talks NMEA 0183; only the
 * wiring and the baud rate change.
 */
#include "esp_log.h"

#include "espos.h"
#include "espos_nmea0183_sk.h"
#include "espos_nmea0183_uart.h"

static const char *TAG = "nmea0183_gps";

/* The GPS module's TX goes to this pin. Free pins per chip, as in
 * docs/sensors.md; on a Waveshare P4 panel stay off GPIO 14-19 (the radio). */
#if CONFIG_IDF_TARGET_ESP32P4
#define GPS_RX_GPIO 32
#elif CONFIG_IDF_TARGET_ESP32
#define GPS_RX_GPIO 16
#elif CONFIG_IDF_TARGET_ESP32S3
#define GPS_RX_GPIO 18
#else
#define GPS_RX_GPIO 5
#endif

static void on_sentence(const char *raw, const espos_nmea0183_sentence_t *s, void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "%s", raw);
    /* The clock first, so the deltas of this very sentence carry GPS time. */
    espos_nmea0183_sk_publish(s, true);
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));

    /* u-blox NEO-6M/M8N, ATGM336H and most other 3.3 V modules: 9600 baud,
     * not inverted. A marine NMEA 0183 talker: 4800 through an RS-422
     * receiver (docs/nmea0183.md). */
    const espos_nmea0183_uart_cfg_t cfg = {
        .uart_num = 1,
        .rx_gpio = GPS_RX_GPIO,
        .tx_gpio = -1,
        .baud = 9600,
    };
    espos_nmea0183_uart_t gps;
    ESP_ERROR_CHECK(espos_nmea0183_uart_open(&cfg, &gps));
    if (espos_nmea0183_uart_add_listener(gps, on_sentence, NULL) == 0) {
        ESP_LOGE(TAG, "no listener slot");
    }
}
