# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_core is the boot, espos_sdlog the card and the sessions on it, and
# espos_time the clock each row is stamped with.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_sdlog espos_time esp_system)
