# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The radio co-processor image an ESP32-P4 espOS board installs over its
# SDIO link (POST /api/v1/system/coprocessor/update). Not part of the espOS
# build: built on its own by .github/workflows/coprocessor.yml.
cmake_minimum_required(VERSION 3.16)

# The esp_hosted version, which is also what the image reports to the host.
# Keep in step with main/idf_component.yml and
# ESPOS_COPROC_FW_VERSION in components/espos_wifi/src/coproc_image.h.
set(PROJECT_VER "3.0.9")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espos_coprocessor)
