# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# The pure sources only: the card is driver plumbing with nothing to decide.
set(sdlog_src "${CMAKE_CURRENT_LIST_DIR}/../../../../components/espos_sdlog")
idf_component_register(
    SRCS test_main.c test_sha256.c test_session.c
         "${sdlog_src}/src/sdlog_session.c" "${sdlog_src}/src/sdlog_sha256.c"
    INCLUDE_DIRS "." "${sdlog_src}/include"
    PRIV_REQUIRES unity
    WHOLE_ARCHIVE)
