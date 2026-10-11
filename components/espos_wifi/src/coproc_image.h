// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
/*
 * The one radio co-processor image this firmware installs
 * (POST /api/v1/system/coprocessor/update, coproc_update.c).
 *
 * Source, not Kconfig: an existing sdkconfig keeps the value it was first
 * written with even after a Kconfig default changes, so a pin there would
 * leave every checkout that bumps espOS offering the previous image against
 * the new host.
 *
 * The image carries no signature this device can check, so the hash is what
 * makes it trusted: this firmware is signed, and it accepts exactly the image
 * with this SHA-256 from whichever URL it was fetched. The coprocessor
 * workflow (.github/workflows/coprocessor.yml) rebuilds tools/coprocessor
 * reproducibly, fails when the result is not this hash, and its error says
 * what to set. The file name carries the hash's first 12 hex digits, so an
 * image with different bytes is published beside the old one, never over it.
 */
#pragma once

/* The esp_hosted version the image is built from: the esp_hosted pin in this
 * component's idf_component.yml, checked against the host at compile time. */
#define ESPOS_COPROC_FW_VERSION       "3.0.9"
#define ESPOS_COPROC_FW_VERSION_MAJOR 3
#define ESPOS_COPROC_FW_VERSION_MINOR 0
#define ESPOS_COPROC_FW_VERSION_PATCH 9
#define ESPOS_COPROC_FW_SHA256        "33154c51f39792b23bdcb456975149b3f57b2062918843f933e4f2834ce0b9fa"
/* Used when the update request names no URL; a client such as the Signal K
 * manager may pass its own mirror instead. */
#define ESPOS_COPROC_FW_URL                                                                   \
    "https://raw.githubusercontent.com/signalk-espOS/espOS/coprocessor-assets/esp32c6/3.0.9/" \
    "espos-coprocessor-esp32c6-3.0.9-33154c51f397.bin"
