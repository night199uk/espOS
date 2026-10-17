# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The pure sources only, the way espos_power_test does it: the UART is driver
# plumbing with nothing to decide, and the publisher is a loop over the map.
set(nmea_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_nmea0183")
idf_component_register(
    SRCS test_main.c test_parse.c test_decode.c test_sk_map.c
         "${nmea_src}/src/nmea0183_parse.c" "${nmea_src}/src/nmea0183_sk_map.c"
    INCLUDE_DIRS "." "${nmea_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)
