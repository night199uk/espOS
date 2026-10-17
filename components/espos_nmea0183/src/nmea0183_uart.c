/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The UART half of espos_nmea0183: a task per port feeding the reader. The
 * framing and the parsing it calls are pure C and host-tested; this file is
 * only the driver plumbing around them.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espos_nmea0183_uart.h"

static const char *TAG = "nmea0183";

#define RX_BUF     1024
#define READ_CHUNK 128

typedef struct {
    int id;
    espos_nmea0183_sentence_fn fn;
    void *arg;
} listener_t;

struct espos_nmea0183_uart {
    espos_nmea0183_uart_cfg_t cfg;
    espos_nmea0183_reader_t reader;
    espos_nmea0183_uart_stats_t stats;
    /* Held while listeners run, so remove_listener() returning means the
     * listener is not running: the caller may free its argument. */
    SemaphoreHandle_t lock;
    /* The counters have their own, so get_stats() never waits on a listener
     * that is busy writing to a card. */
    portMUX_TYPE stats_mux;
    listener_t listeners[ESPOS_NMEA0183_UART_MAX_LISTENERS];
    int next_id;
    TaskHandle_t task;
    volatile bool stop;
    SemaphoreHandle_t stopped;
    char raw[ESPOS_NMEA0183_LINE_MAX + 1];
};

static void on_line(char *line, size_t len, void *arg)
{
    struct espos_nmea0183_uart *h = arg;
    memcpy(h->raw, line, len + 1);
    espos_nmea0183_sentence_t s;
    espos_nmea0183_err_t err = espos_nmea0183_parse(line, h->cfg.require_checksum, &s);
    taskENTER_CRITICAL(&h->stats_mux);
    h->stats.lines++;
    if (err == ESPOS_NMEA0183_ERR_BAD_CHECKSUM) {
        h->stats.bad_checksum++;
    } else if (err != ESPOS_NMEA0183_OK) {
        h->stats.rejected++;
    } else {
        h->stats.sentences++;
    }
    taskEXIT_CRITICAL(&h->stats_mux);
    if (err == ESPOS_NMEA0183_OK) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        for (int i = 0; i < ESPOS_NMEA0183_UART_MAX_LISTENERS; i++) {
            if (h->listeners[i].fn) {
                h->listeners[i].fn(h->raw, &s, h->listeners[i].arg);
            }
        }
        xSemaphoreGive(h->lock);
    }
}

static void reader_task(void *arg)
{
    struct espos_nmea0183_uart *h = arg;
    uint8_t chunk[READ_CHUNK];
    while (!h->stop) {
        int n = uart_read_bytes(h->cfg.uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (n > 0) {
            espos_nmea0183_reader_feed(&h->reader, chunk, (size_t)n, on_line, h);
            taskENTER_CRITICAL(&h->stats_mux);
            h->stats.overflows = h->reader.overflows;
            h->stats.resyncs = h->reader.resyncs;
            h->stats.garbled = h->reader.garbled;
            taskEXIT_CRITICAL(&h->stats_mux);
        }
    }
    xSemaphoreGive(h->stopped);
    vTaskDelete(NULL);
}

esp_err_t espos_nmea0183_uart_open(const espos_nmea0183_uart_cfg_t *cfg, espos_nmea0183_uart_t *out)
{
    if (!cfg || !out || cfg->rx_gpio < 0 || cfg->uart_num < 0 || cfg->uart_num >= UART_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    struct espos_nmea0183_uart *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->cfg = *cfg;
    if (h->cfg.baud == 0) {
        h->cfg.baud = 4800;
    }
    espos_nmea0183_reader_init(&h->reader);
    portMUX_INITIALIZE(&h->stats_mux);
    h->lock = xSemaphoreCreateMutex();
    h->stopped = xSemaphoreCreateBinary();
    if (!h->lock || !h->stopped) {
        goto fail_mem;
    }

    const uart_config_t uc = {
        .baud_rate = (int)h->cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(h->cfg.uart_num, RX_BUF, h->cfg.tx_gpio >= 0 ? 512 : 0, 0, NULL, 0);
    if (err != ESP_OK) {
        goto fail;
    }
    err = uart_param_config(h->cfg.uart_num, &uc);
    if (err == ESP_OK) {
        err = uart_set_pin(h->cfg.uart_num, h->cfg.tx_gpio >= 0 ? h->cfg.tx_gpio : UART_PIN_NO_CHANGE,
                           h->cfg.rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK && h->cfg.invert_rx) {
        err = uart_set_line_inverse(h->cfg.uart_num, UART_SIGNAL_RXD_INV);
    }
    if (err != ESP_OK) {
        uart_driver_delete(h->cfg.uart_num);
        goto fail;
    }
    if (xTaskCreate(reader_task, "nmea0183", h->cfg.task_stack ? h->cfg.task_stack : 6144, h,
                    h->cfg.task_prio ? (UBaseType_t)h->cfg.task_prio : 5, &h->task) != pdPASS) {
        uart_driver_delete(h->cfg.uart_num);
        goto fail_mem;
    }
    ESP_LOGI(TAG, "UART%d rx %d tx %d at %lu baud%s", (int)h->cfg.uart_num, (int)h->cfg.rx_gpio, (int)h->cfg.tx_gpio,
             (unsigned long)h->cfg.baud, h->cfg.invert_rx ? ", inverted" : "");
    *out = h;
    return ESP_OK;

fail_mem:
    err = ESP_ERR_NO_MEM;
fail:
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    if (h->stopped) {
        vSemaphoreDelete(h->stopped);
    }
    free(h);
    return err;
}

void espos_nmea0183_uart_close(espos_nmea0183_uart_t h)
{
    if (!h) {
        return;
    }
    h->stop = true;
    xSemaphoreTake(h->stopped, portMAX_DELAY);
    uart_driver_delete(h->cfg.uart_num);
    vSemaphoreDelete(h->lock);
    vSemaphoreDelete(h->stopped);
    free(h);
}

int32_t espos_nmea0183_uart_add_listener(espos_nmea0183_uart_t h, espos_nmea0183_sentence_fn fn, void *arg)
{
    if (!h || !fn) {
        return 0;
    }
    int id = 0;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (int i = 0; i < ESPOS_NMEA0183_UART_MAX_LISTENERS; i++) {
        if (!h->listeners[i].fn) {
            id = ++h->next_id;
            h->listeners[i] = (listener_t) { .id = id, .fn = fn, .arg = arg };
            break;
        }
    }
    xSemaphoreGive(h->lock);
    return id;
}

void espos_nmea0183_uart_remove_listener(espos_nmea0183_uart_t h, int32_t id)
{
    if (!h || id <= 0) {
        return;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    for (int i = 0; i < ESPOS_NMEA0183_UART_MAX_LISTENERS; i++) {
        if (h->listeners[i].id == id) {
            memset(&h->listeners[i], 0, sizeof(h->listeners[i]));
        }
    }
    xSemaphoreGive(h->lock);
}

esp_err_t espos_nmea0183_uart_write(espos_nmea0183_uart_t h, const char *line)
{
    if (!h || !line) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->cfg.tx_gpio < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    char buf[ESPOS_NMEA0183_LINE_MAX + 3];
    size_t n = espos_nmea0183_finish(line, buf, sizeof(buf));
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return uart_write_bytes(h->cfg.uart_num, buf, n) == (int)n ? ESP_OK : ESP_FAIL;
}

void espos_nmea0183_uart_get_stats(espos_nmea0183_uart_t h, espos_nmea0183_uart_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!h) {
        return;
    }
    taskENTER_CRITICAL(&h->stats_mux);
    *out = h->stats;
    taskEXIT_CRITICAL(&h->stats_mux);
}
