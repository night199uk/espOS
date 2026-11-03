# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Compiles the state machine sources directly: no espos_config/httpd needed.
set(wifi_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_wifi")
idf_component_register(
    SRCS test_main.c test_sm.c test_last_ap.c test_ps.c test_stall.c test_fresh.c "${wifi_src}/src/wifi_sm.c" "${wifi_src}/src/wifi_reason.c" "${wifi_src}/src/wifi_last_ap.c" "${wifi_src}/src/wifi_ps.c" "${wifi_src}/src/wifi_stall.c" "${wifi_src}/src/wifi_fresh.c"
    INCLUDE_DIRS "." "${wifi_src}/include" "${wifi_src}/src"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)
