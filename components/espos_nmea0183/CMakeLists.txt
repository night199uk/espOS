# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_nmea0183: NMEA 0183 sentences off a UART, and what they mean in
# Signal K. Optional: name it in espos_project(COMPONENTS ...).
#
# The framing, the decoders and the Signal K mapping are pure C
# (nmea0183_parse.c, nmea0183_sk_map.c), host-tested in
# test/host/espos_nmea0183_test; the UART is behind nmea0183_uart.c, with a
# stub on the linux target.
idf_build_get_property(target IDF_TARGET)
set(srcs src/nmea0183_parse.c src/nmea0183_sk_map.c src/nmea0183_sk_publish.c)
set(priv_reqs)
if(target STREQUAL "linux")
    list(APPEND srcs src/nmea0183_uart_sim.c)
else()
    list(APPEND srcs src/nmea0183_uart.c)
    list(APPEND priv_reqs esp_driver_uart)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES esp_common
    PRIV_REQUIRES ${priv_reqs})

# The Signal K publisher and the GPS clock are used when espos_sk and
# espos_time are in the build; a firmware that only logs sentences links
# neither. Namespace-tolerant checks (espOS #138), from this component's own
# copy of the helpers, because a registry install cannot reach a sibling's.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/espos_components.cmake")

espos_optional_requires(PRIVATE espos_sk espos_time)
espos_has_component(_have_sk espos_sk)
if(_have_sk)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_NMEA0183_HAVE_SK=1)
endif()
espos_has_component(_have_time espos_time)
if(_have_time)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_NMEA0183_HAVE_TIME=1)
endif()
