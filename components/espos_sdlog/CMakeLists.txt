# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_sdlog: a microSD card, and sessions of rows on it that survive a
# power cut. Optional: name it in espos_project(COMPONENTS ...).
#
# The sessions and the hash are pure C on stdio (sdlog_session.c,
# sdlog_sha256.c), host-tested in test/host/espos_sdlog_test; the card is
# behind sdlog_card.c, with a stub on the linux target.
idf_build_get_property(target IDF_TARGET)
set(srcs src/sdlog_session.c src/sdlog_sha256.c)
set(priv_reqs)
if(target STREQUAL "linux")
    list(APPEND srcs src/sdlog_card_sim.c)
else()
    list(APPEND srcs src/sdlog_card.c)
    list(APPEND priv_reqs fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES esp_common
    PRIV_REQUIRES ${priv_reqs})
