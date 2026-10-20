# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Host unit test: the delta window's configured pending cap.
#   idf.py --preview set-target linux && idf.py build && ./build/espos_sk_pending_test.elf
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(espos_sk_pending_test)
