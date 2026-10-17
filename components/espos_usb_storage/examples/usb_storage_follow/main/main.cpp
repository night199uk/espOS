/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * usb_storage_follow -- the board's microSD card appears to a chartplotter as
 * a second card on its USB port, and every new RealBathy record the plotter
 * writes there is handed to a sink once the plotter has gone quiet.
 *
 * The sink here only logs. A real firmware uploads the chunk and returns true
 * only once the server has it; returning false keeps the bytes for the next
 * pass, which is how a boat out of WiFi range loses nothing.
 *
 * Pins are the Waveshare ESP32-P4 boards' TF slot (the PoE-ETH and the
 * Touch-LCD panels share them). The plotter connects to the P4's USB 2.0 OTG
 * high-speed port; on the PoE-ETH that is the 4-pin header.
 */
#include <cinttypes>
#include <utility>

#include "esp_log.h"
#include "espos.h"
#include "espos_usb_storage/usb_storage.h"

namespace {

const char* TAG = "usb_storage_follow";

/* RealBathy writes realbathy/<id>.trl: fixed 24-byte records (position,
 * depth, time), appended while the plotter records. */
constexpr uint32_t kTrlRecord = 24;

}  // namespace

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(espos_start(nullptr));

  espos_usb_storage::Config cfg = {};
  cfg.sd = {.clk = 43,
            .cmd = 44,
            .d0 = 39,
            .d1 = 40,
            .d2 = 41,
            .d3 = 42,
            .slot = 0,
            .freq_khz = 40000,
            .ldo_chan = 4};
  if (espos_usb_storage::start(cfg) != ESP_OK) {
    ESP_LOGE(TAG, "no card exported: check that a FAT32 microSD is inserted");
    return;
  }

  espos_usb_storage::FollowConfig follow;
  follow.follower.watches = {{"realbathy", ".trl", kTrlRecord}};
  follow.sink = [](const espos_usb_storage::Chunk& c) {
    ESP_LOGI(TAG, "%s: %u records from byte %" PRIu64 "%s", c.path.c_str(),
             static_cast<unsigned>(c.len / kTrlRecord), c.offset,
             c.restarted ? " (file replaced)" : "");
    return true;
  };
  ESP_ERROR_CHECK(espos_usb_storage::follow(std::move(follow)));
}
