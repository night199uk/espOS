# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Everything before project() is the shared prologue (docs/development.md).
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
# espos_usb_storage is optional, so it has to be named: projects on espOS build
# with IDF's MINIMAL_BUILD, and a component nothing requires is not compiled.
espos_project_prologue(NAME "usb_storage_follow" COMPONENTS espos_usb_storage)
project(usb_storage_follow)
espos_project_ui_partition()
