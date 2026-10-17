# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The follower and the progress store are compiled directly: they are
# driver-free by design, while the component around them needs TinyUSB and the
# SDMMC host the linux target has not got. No port_sim: the follower's port is
# the FsView interface, which test_follower.cpp fakes, and the store runs
# against the linux target's real NVS.
set(src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_usb_storage")
idf_component_register(
    SRCS test_main.c test_follower.cpp test_nvs_progress.cpp
         "${src}/src/follower.cpp" "${src}/src/nvs_progress.cpp"
    INCLUDE_DIRS "." "${src}/include" "${src}/src"
    PRIV_REQUIRES unity nvs_flash esp_partition
    WHOLE_ARCHIVE)
