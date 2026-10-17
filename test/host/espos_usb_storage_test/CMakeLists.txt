# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
# Host unit tests for espos_usb_storage's follower and progress store: which
# bytes of a file the USB host is writing get handed off, when, and exactly
# once, and what survives a reboot.
#   idf.py --preview set-target linux && idf.py build && ./build/espos_usb_storage_test.elf
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(espos_usb_storage_test)
