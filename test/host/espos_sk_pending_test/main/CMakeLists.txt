# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
set(sk_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_sk")
idf_component_register(
    SRCS test_main.c test_pending.c "${sk_src}/src/sk_delta.c"
    INCLUDE_DIRS "." "${sk_src}/include"
    PRIV_REQUIRES unity esp_common
    WHOLE_ARCHIVE)
# Exercise the CONFIG_ESPOS_SK_PENDING_MAX branch with a reduced cap. The main
# espos_sk_test builds without a Kconfig and covers the default; keep CAP in
# test_pending.c in step with this value.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_ESPOS_SK_PENDING_MAX=4)
