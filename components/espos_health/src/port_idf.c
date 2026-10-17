/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chip side of health_port.h: esp_timer for the tick, heap_caps for the
 * readings, the IDF task watchdog, and a reset record in memory that survives
 * esp_restart().
 */
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif

#include "health_port.h"

/* RTC memory where the chip has it; the ESP32-C2 has none, and its plain
 * .noinit SRAM keeps its contents across a software reset just the same. */
#if CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
#define RECORD_ATTR RTC_NOINIT_ATTR
#else
#define RECORD_ATTR __NOINIT_ATTR
#endif

#define RECORD_MAGIC 0x6865616Cu /* health_policy.c writes it; the value is the contract */

static RECORD_ATTR espos_health_reset_record_t s_record;

uint32_t espos_health_port_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t espos_health_port_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

int64_t espos_health_port_unix_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    /* Before 2020 the clock was never set: the epoch, or a fantasy. */
    if (tv.tv_sec < 1577836800) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void espos_health_port_heap(espos_health_heap_t *out)
{
    out->total_free = esp_get_free_heap_size();
    out->total_min = esp_get_minimum_free_heap_size();
    /* 8BIT as well as INTERNAL: on a classic ESP32 the IRAM left over after
     * code is heap too, but 32-bit access only, so no byte buffer, stack or
     * WiFi frame can come from it. Counting it reported ~50 KB internal free
     * on a HALMET whose malloc() heap was down to 18 KB, and the alarm that
     * finally fired blamed fragmentation for what was exhaustion. */
    out->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void espos_health_port_restart(void)
{
    /* Let the log lines that explain this leave the UART first. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void espos_health_port_record_store(const espos_health_reset_record_t *rec)
{
    s_record = *rec;
}

bool espos_health_port_record_take(espos_health_reset_record_t *out)
{
    bool ours = false;
    if (s_record.magic == RECORD_MAGIC) {
        /* Only a software reset can be the policy's; a record still in memory
         * after a power glitch or a panic is from an older boot. */
        ours = esp_reset_reason() == ESP_RST_SW;
        if (ours) {
            *out = s_record;
        }
    }
    memset(&s_record, 0, sizeof(s_record));
    return ours;
}

esp_err_t espos_health_port_tick_start(uint32_t period_ms, void (*cb)(void *arg), void *arg)
{
    const esp_timer_create_args_t args = {
        .callback = cb,
        .arg = arg,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "espos_health",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t t = NULL;
    esp_err_t err = esp_timer_create(&args, &t);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(t, (uint64_t)period_ms * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(t);
    }
    return err;
}

#if CONFIG_ESP_TASK_WDT_EN

esp_err_t espos_health_port_twdt_add(void)
{
    esp_err_t st = esp_task_wdt_status(NULL);
    if (st == ESP_OK) {
        return ESP_OK; /* already subscribed */
    }
    if (st == ESP_ERR_INVALID_STATE) {
        return st; /* the watchdog is not running (CONFIG_ESP_TASK_WDT_INIT off) */
    }
    return esp_task_wdt_add(NULL);
}

esp_err_t espos_health_port_twdt_delete(void)
{
    return esp_task_wdt_delete(NULL);
}

void espos_health_port_twdt_reset(void)
{
    /* ESP_ERR_NOT_FOUND for a task that is not subscribed: kick() is allowed
     * from anywhere, so that is not worth a log line. */
    (void)esp_task_wdt_reset();
}

#else

esp_err_t espos_health_port_twdt_add(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espos_health_port_twdt_delete(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void espos_health_port_twdt_reset(void)
{
}

#endif

void *espos_health_port_self(void)
{
    return xTaskGetCurrentTaskHandle();
}
