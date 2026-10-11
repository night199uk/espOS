/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_hosted's Wi-Fi + BLE co-processor (examples/bluetooth/esp_hosted_nimble/
 * bleprph_wifi_coex/cp): the transport, Wi-Fi and the BLE controller all start
 * from esp_hosted's own init, so the application only brings up NVS and
 * confirms the image.
 */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "espos_cp";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    /* The bootloader on the C6 is whatever it shipped with, and an update over
     * the link does not replace it. If that one was built with app rollback,
     * an image still unconfirmed at its second boot is rolled back. Waiting
     * for a host to negotiate first would roll back every update: the first
     * boot after one is the C6 restarting itself, the P4 then loses the link
     * and restarts, and the P4 resets this chip at every start, so the first
     * boot a host talks to is the second. Confirming means only that the image
     * boots and opens NVS; without rollback it just marks its slot valid. */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "espOS co-processor (esp_hosted Wi-Fi + BLE) running");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
