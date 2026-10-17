/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_USB_STORAGE_CARD_LOCK_H_
#define ESPOS_USB_STORAGE_CARD_LOCK_H_

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "sd_protocol_types.h"

/// Who touches the exported card, and when.
///
/// esp_tinyusb writes the host's sectors with sdmmc_write_sectors(), deferred
/// to its own task, so no TinyUSB callback marks the moment the card changes.
/// The link wraps that function instead (CMakeLists.txt): every write to the
/// exported card takes this lock and bumps the generation first, and espOS's
/// own reads take the same lock. A read then never lands while the card is
/// still programming a host write, and a reader that sees the generation
/// unchanged knows no write happened between its reads.
namespace espos_usb_storage::card_lock {

/// Starts tracking `card`. Before the card is exported.
void track(sdmmc_card_t* card);

uint32_t generation();
int64_t last_write_us();

/// sdmmc_read_sectors() under the lock.
esp_err_t read(sdmmc_card_t* card, void* dst, size_t start, size_t count);

}  // namespace espos_usb_storage::card_lock

#endif  // ESPOS_USB_STORAGE_CARD_LOCK_H_
