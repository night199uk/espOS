# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Compile the component's pure-logic sources directly rather than depending on
# espos_ble: the component pulls in Bluetooth and networking, neither of which
# exists on the linux target.
set(ble_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_ble")
idf_component_register(
    SRCS test_main.c test_hex.c test_advq.c test_post_sched.c test_write_mode.c test_uuid.c "${ble_src}/src/ble_proto.c"
    INCLUDE_DIRS "." "${ble_src}/include" "${ble_src}/src"
    PRIV_REQUIRES unity espressif__cjson
    WHOLE_ARCHIVE)
