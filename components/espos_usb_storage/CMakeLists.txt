# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# A microSD card exported to a USB host (a chartplotter, a PC) as an ordinary
# storage device, while espOS reads what the host writes there and hands new
# data on -- without ever writing to the card itself. docs/usb-storage.md.
#
# The export needs USB device mode and an SDMMC host on one chip: the ESP32-P4
# and the ESP32-S3. Elsewhere only the follower builds, which is driver-free and
# is what the host tests compile.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32p4" OR target STREQUAL "esp32s3")
    idf_component_register(
        SRCS src/follower.cpp src/usb_storage.cpp src/fs_view.cpp
             src/nvs_progress.cpp src/card_lock.cpp
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS src
        PRIV_REQUIRES espressif__esp_tinyusb esp_driver_sdmmc sdmmc fatfs
                      nvs_flash esp_timer)
    # The host's writes reach the card through esp_tinyusb's own call to
    # sdmmc_write_sectors(), deferred to its task. Wrapping that call is the
    # one place every write can be seen and kept apart from espOS's reads
    # (src/card_lock.h). The -u pulls card_lock.cpp
    # in even when nothing calls into it, because once the wrap is on, every
    # sdmmc_write_sectors() in the firmware needs __wrap_ to exist.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_write_sectors" "-u __wrap_sdmmc_write_sectors")
else()
    idf_component_register(
        SRCS src/follower.cpp
        INCLUDE_DIRS include)
endif()
