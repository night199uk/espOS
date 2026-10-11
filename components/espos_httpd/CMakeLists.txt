# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
idf_build_get_property(target IDF_TARGET)
set(priv_reqs esp_timer esp_app_format esp_system esp_event espos_log espos_event espos_health espressif__cjson)
if(NOT target STREQUAL "linux")
    # spi_flash/esp_hw_support/heap: the hardware block in api_system.c
    # (flash size, base MAC, RAM totals). nvs_flash: the power-cycle counter
    # behind the portal recovery window in auth.c, which has to survive the
    # power cycles it counts. Device-only, like the rest of this list -- the
    # linux target has none of them and the code is #if-guarded.
    list(APPEND priv_reqs esp_netif espcoredump esp_partition joltwallet__littlefs
                          spi_flash esp_hw_support heap nvs_flash)
endif()

# The web UI bundle, built into the app image. An OTA update writes only the
# app slot, so a UI served from the "storage" partition stayed at whatever a
# USB flash put there while the firmware moved on: a fixed page never reached
# an OTA-updated device. ui_bundle.c is the table static.c serves from.
set(ui_files "")
if(CONFIG_ESPOS_HTTPD_UI_EMBED)
    file(GLOB_RECURSE ui_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/ui-dist/*")
    list(SORT ui_files)
endif()
set(ui_decls "")
set(ui_rows "")
set(ui_index 0)
foreach(f ${ui_files})
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_LIST_DIR}/ui-dist" "${f}")
    set(gz false)
    if(rel MATCHES "^(.*)\\.gz$")
        set(rel "${CMAKE_MATCH_1}")
        set(gz true)
    endif()
    string(APPEND ui_decls "extern const uint8_t ui${ui_index}_start[] asm(\"_binary_espos_ui_${ui_index}_start\");\n")
    string(APPEND ui_decls "extern const uint8_t ui${ui_index}_end[] asm(\"_binary_espos_ui_${ui_index}_end\");\n")
    string(APPEND ui_rows "    { \"/${rel}\", ui${ui_index}_start, ui${ui_index}_end, ${gz} },\n")
    math(EXPR ui_index "${ui_index} + 1")
endforeach()
if(ui_index EQUAL 0)
    set(ui_rows "    { 0 },\n") # an empty initializer list is not C
endif()
set(ui_table "${CMAKE_CURRENT_BINARY_DIR}/ui_bundle.c")
# file(CONFIGURE) rewrites only on a change, so an unchanged bundle does not
# rebuild the component. Not in the requirements pass, which has no sdkconfig.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
file(CONFIGURE OUTPUT "${ui_table}" CONTENT "/* Generated from ui-dist/ by espos_httpd/CMakeLists.txt. */
#include \"${CMAKE_CURRENT_LIST_DIR}/src/ui_bundle.h\"

${ui_decls}
const espos_ui_file_t espos_ui_files[] = {
${ui_rows}};
const size_t espos_ui_file_count = ${ui_index};
" @ONLY)
endif()

idf_component_register(
    SRCS src/espos_httpd.c src/api_config.c src/api_system.c src/api_logs.c src/api_coredump.c src/api_health.c src/static.c src/sse.c
         src/auth.c src/auth_policy.c "${ui_table}"
    INCLUDE_DIRS include
    REQUIRES esp_http_server espos_config
    PRIV_REQUIRES ${priv_reqs}
    EMBED_TXTFILES www/index.html)

set(ui_index 0)
foreach(f ${ui_files})
    target_add_binary_data(${COMPONENT_LIB} "${f}" BINARY RENAME_TO "espos_ui_${ui_index}")
    math(EXPR ui_index "${ui_index} + 1")
endforeach()

espos_config_add_descriptor(config/httpd.json)
