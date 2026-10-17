/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_USB_STORAGE_USB_STORAGE_H_
#define ESPOS_USB_STORAGE_USB_STORAGE_H_

#include <cstdint>

#include "esp_err.h"
#include "espos_usb_storage/follower.h"

/// A microSD card exported to a USB host as an ordinary storage device, with
/// espOS reading what the host writes.
///
/// The host owns the filesystem for as long as the card is exported. espOS
/// never writes to it and never mounts it read-write; it watches the host's
/// writes go by and, once they have stopped for a while, reads the card
/// through a read-only FAT view of its own (docs/usb-storage.md).
namespace espos_usb_storage {

/// The SDMMC slot the card sits in. No defaults: which pins carry the card is
/// a property of the board.
struct SdConfig {
  int clk;
  int cmd;
  int d0;
  int d1 = -1;  ///< -1 with d2/d3 for a 1-bit bus.
  int d2 = -1;
  int d3 = -1;
  int slot = 0;
  /// kHz; 40000 is SDMMC_FREQ_HIGHSPEED. Long ribbon cables may need 20000.
  int freq_khz = 40000;
  /// ESP32-P4 boards power the SD I/O rail from an on-chip LDO channel
  /// (4 on the Waveshare P4 boards); -1 for a board that does not.
  int ldo_chan = -1;
};

/// Which USB controller faces the host.
enum class UsbPort {
  /// High-speed on the ESP32-P4, full-speed on the ESP32-S3.
  kDefault,
  /// The P4's USB 2.0 OTG high-speed port (480 Mbit/s).
  kHighSpeed,
  /// The full-speed OTG port (12 Mbit/s): the S3's only one, and on the P4 the
  /// PHY the USB-Serial/JTAG console otherwise uses, so a board whose USB-C
  /// goes straight to the chip loses that console while the card is exported.
  kFullSpeed,
};

struct Config {
  SdConfig sd;
  UsbPort port = UsbPort::kDefault;
};

/// Mounts nothing: brings up the card and exports it over USB device mode.
esp_err_t start(const Config& cfg);

/// Counts the host's writes to the card, bumped before each one lands. A
/// reader that sees it change knows its view of the card may be stale.
uint32_t write_generation();

/// esp_timer time the last host write finished, or 0 if there was none.
int64_t last_write_us();

/// Whether a USB host has configured the device.
bool host_connected();

struct FollowConfig {
  FollowerConfig follower;
  /// The host must have been quiet this long before a pass starts.
  uint32_t quiet_ms = 15000;
  /// How often to look again while there is nothing to do.
  uint32_t poll_ms = 10000;
  ChunkSink sink;
  /// NVS namespace for progress; one per follower.
  const char* nvs_namespace = "usbst_follow";
};

/// Starts a task that follows the watched files and hands new records to
/// `cfg.sink` on that task, so the sink may block on the network. Requires
/// start() first.
esp_err_t follow(FollowConfig cfg);

}  // namespace espos_usb_storage

#endif  // ESPOS_USB_STORAGE_USB_STORAGE_H_
