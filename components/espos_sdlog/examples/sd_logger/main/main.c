/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * A row a second onto a microSD card, in sessions that survive a power cut:
 * mount, close whatever the last power cut left open, then log. Pull the
 * plug at any moment and the next boot finds every complete row, hashed.
 *
 * What a row holds is the firmware's; this one logs the clock and the free
 * heap, a depth logger logs a sounding with its position.
 */
#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espos.h"
#include "espos_sdlog.h"
#include "espos_sdlog_card.h"
#include "espos_time.h"

static const char *TAG = "sd_logger";

#define ROOT "/sdcard/logs"
/* A session an hour: a file small enough to upload over a weak link, and a
 * power cut costs at most the rows since the last sync of one of them. */
#define ROWS_PER_SESSION 3600
#define RETRY_MS         10000

static espos_sdlog_card_cfg_t card_cfg(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    return ESPOS_SDLOG_CARD_WAVESHARE_P4();
#elif CONFIG_IDF_TARGET_ESP32
    return ESPOS_SDLOG_CARD_SPI(1, 23, 19, 18, 5);
#elif CONFIG_IDF_TARGET_ESP32S3
    return ESPOS_SDLOG_CARD_SPI(1, 11, 13, 12, 10);
#else
    return ESPOS_SDLOG_CARD_SPI(1, 7, 2, 6, 10);
#endif
}

static bool print_session(const espos_sdlog_info_t *info, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "session %08" PRIu32 ": %s%s, %" PRIu64 " rows%s", info->seq, info->closed ? "closed" : "open",
             info->closed && !info->clean ? " after a power cut" : "", info->rows, info->synced ? ", synced" : "");
    return true;
}

static espos_sdlog_session_t start_session(void)
{
    char header[96];
    snprintf(header, sizeof(header), "{\"started_ms\":%" PRId64 ",\"columns\":[\"time_ms\",\"free_heap\"]}",
             espos_time_now_ms());
    espos_sdlog_session_t s = NULL;
    esp_err_t err = espos_sdlog_open(ROOT, header, NULL, &s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot start a session: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "logging to session %08" PRIu32, espos_sdlog_seq(s));
    return s;
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));

    const espos_sdlog_card_cfg_t cfg = card_cfg();
    esp_err_t err = espos_sdlog_card_mount(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no card: %s", esp_err_to_name(err));
        return;
    }
    uint32_t recovered = 0, removed = 0;
    espos_sdlog_recover(ROOT, &recovered, &removed);
    if (recovered || removed) {
        ESP_LOGW(TAG, "closed %" PRIu32 " session(s) a power cut left open, removed %" PRIu32, recovered, removed);
    }
    espos_sdlog_list(ROOT, print_session, NULL);

    /* A card that refuses one write may take the next: a logger out of
     * reach for a week keeps trying rather than stopping for good. */
    espos_sdlog_session_t s = NULL;
    for (;;) {
        if (!s && !(s = start_session())) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        char row[48];
        /* 0 until the clock is set (SNTP, the server, a GPS): an honest gap
         * rather than a time that looks right and is not. */
        snprintf(row, sizeof(row), "%" PRId64 ",%" PRIu32, espos_time_now_ms(), esp_get_free_heap_size());
        if (espos_sdlog_append(s, row) != ESP_OK) {
            ESP_LOGE(TAG, "the card refused a row; closing the session");
            espos_sdlog_close(s, NULL);
            s = NULL;
        } else if (espos_sdlog_rows(s) >= ROWS_PER_SESSION) {
            espos_sdlog_close(s, NULL);
            s = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
