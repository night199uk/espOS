# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Everything before project() is the shared prologue: the IDF version policy,
# espOS's sdkconfig defaults and partition table, the app-signing key. Used
# outside the espOS tree the include becomes
# "${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake" with espOS as
# the `espos/` submodule (docs/development.md).
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
# espos_sdlog is optional, so it has to be named.
espos_project_prologue(NAME "sd_logger" COMPONENTS espos_sdlog)
project(sd_logger)
# The committed web UI bundle -> LittleFS "storage" image.
espos_project_ui_partition()
