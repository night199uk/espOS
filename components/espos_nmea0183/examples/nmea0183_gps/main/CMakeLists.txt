# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_core is the boot, espos_nmea0183 reads the GPS, espos_sk is where the
# values go and espos_time the clock a fix sets.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_nmea0183 espos_sk espos_time)
