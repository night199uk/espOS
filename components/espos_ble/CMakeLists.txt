# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)

# ble_proto.c is pure logic and builds anywhere (it is what test/host exercises);
# everything else needs a Bluetooth stack, which the linux target has not got.
set(srcs src/ble_proto.c)
set(priv_reqs espressif__cjson espos_event espos_net)

# Target, not CONFIG_BT_BLUEDROID_ENABLED: component CMakeLists are evaluated
# during early expansion, before sdkconfig exists, so a CONFIG_ guard here is
# empty on the first pass - it drops `bt` from the requirements while the
# sources still compile on the second, and the build fails on a missing
# esp_gattc_api.h. The sources are #if'd on the symbol instead, which costs a
# few empty objects on a target built without Bluetooth.
if(NOT target STREQUAL "linux")
    list(APPEND srcs src/ble_backend.c src/ble_gattc.c src/ble_gateway.c src/api_ble.c)
    list(APPEND priv_reqs bt esp_http_client esp_websocket_client esp_timer esp_event nvs_flash)
endif()

# The ESP32-P4 has no radio: Bluedroid's HCI is routed at the C6 co-processor
# through esp_hosted, so the backend includes its headers on that target only.
if(target STREQUAL "esp32p4")
    list(APPEND priv_reqs espressif__esp_hosted)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_config espos_httpd espos_sk
    PRIV_REQUIRES ${priv_reqs})

espos_config_add_descriptor(config/ble.json)
