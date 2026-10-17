# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Host (linux target) unit tests for espos_sdlog: sessions, recovery after a
# power cut, sync markers, pruning and the sha256, on a temporary directory.
# Build & run:
#
#   ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build && ./build/espos_sdlog_test.elf
#
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components")
project(espos_sdlog_test)
