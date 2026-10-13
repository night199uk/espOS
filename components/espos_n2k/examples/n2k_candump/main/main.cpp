/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * n2k_candump — an NMEA 2000 gateway: CAN frames off the boat's bus, out over
 * TCP in candump ASCII, which signalk-server reads with canboatjs.
 *
 * The device decodes nothing. It is a wire, and that is the point: PGN
 * decoding changes far more often than firmware should, so canboat owns it on
 * the server and this stays a pipe. Add the device in signalk-server as an
 * "NMEA 2000 IP gateway" with format `candump3`, pointing at this device's
 * address on port 2599.
 *
 * WIRING IS THE PART THAT GOES WRONG. A CAN bus needs a transceiver — the
 * ESP32's TWAI peripheral is TTL and cannot drive a differential pair — and
 * it needs termination. On a boat the backbone is already terminated at both
 * ends, so a drop cable adds none. On a bench you need 120 Ohm at each end or
 * nothing will be received, which looks exactly like a software fault.
 *
 * GET /api/v1/n2k tells the two apart without a serial cable:
 *
 *   frames: 0, errors: 0        nothing is transmitting, or you are not
 *                               connected to the bus at all
 *   frames: 0, errors climbing  you are on the bus and cannot understand it:
 *                               wrong bitrate, or a broken pair
 *   frames climbing             it works
 */
#include "esp_log.h"
#include "espos.h"
#include "espos_n2k/candump_tcp_server.h"
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/twai_transmitter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char* TAG = "n2k_candump";

/* The board's CAN pins. There is no sensible default -- which GPIOs carry CAN
 * is a property of the hardware, and a number that exists on one target does
 * not on another -- so espos_n2k refuses to guess and logs instead of binding
 * the wrong pin. These are for the Waveshare ESP32-P4 boards; change them. */
constexpr gpio_num_t kCanTx = GPIO_NUM_20;
constexpr gpio_num_t kCanRx = GPIO_NUM_21;

/* NMEA 2000 is 250 kbit/s. It is not negotiable on a real bus: a device at the
 * wrong rate does not simply miss frames, it corrupts them for everyone else
 * by acknowledging at the wrong time. */
constexpr uint32_t kBitrate = 250000;

}  // namespace

extern "C" void app_main(void) {
  /* log -> config -> httpd -> wifi -> sk -> ota, in the one order that
   * works. The network comes up in the background; the bus does not depend
   * on it, so frames are counted from the moment the transceiver sees
   * traffic whether or not a server is reachable. */
  ESP_ERROR_CHECK(espos_start(nullptr));

  /* static, because the server keeps pointers to these and all three outlive
   * app_main(); a stack object here would be destroyed under a running
   * task. Nothing needs to own them -- the device runs until it reboots. */
  static espos_n2k::TwaiReceiver rx({
      .tx_pin = kCanTx,
      .rx_pin = kCanRx,
      .bitrate = kBitrate,
  });
  static espos_n2k::TwaiTransmitter tx;

  /* The receiver owns the bus; the transmitter attaches to whatever the
   * receiver brought up. Starting the transmitter first logs an error and
   * does nothing, which is the honest failure for "no bus yet". */
  rx.start();
  tx.start();

  /* Passing the transmitter is what makes this bidirectional: a candump line
   * arriving from the server is written to the bus. Pass nullptr instead and
   * the gateway is read-only -- worth doing on a boat where nothing should
   * ever originate frames from this device. */
  static espos_n2k::CandumpTcpServer server(&rx, &tx, {});
  server.start();

  ESP_LOGI(TAG, "CAN on TX=%d RX=%d at %u kbit/s; candump on port %d",
           (int)kCanTx, (int)kCanRx, (unsigned)(kBitrate / 1000),
           (int)CONFIG_ESPOS_N2K_CANDUMP_PORT);
  ESP_LOGI(TAG, "bus health: GET /api/v1/n2k");

  /* Nothing to do here: frames are handled on the shared TWAI node's task
   * and fanned out to client queues. This loop only narrates, and only when
   * something is worth saying -- a gateway that logs every frame is a
   * gateway nobody can read the log of. */
  bool announced = false;
  bool warned_quiet = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (!announced && rx.ever_received()) {
      announced = true;
      ESP_LOGI(TAG, "bus is live: %u frames", (unsigned)rx.frames_received());
    }
    /* A bus that was live and went quiet is worth saying, because from the
     * outside it is indistinguishable from a working gateway. Once per
     * outage, not once per tick: a warning every ten seconds is how a log
     * stops being read. The flag clears when frames return, so the next
     * outage warns again. */
    const int64_t idle = rx.seconds_since_last_rx();
    if (announced && idle > 30) {
      if (!warned_quiet) {
        warned_quiet = true;
        ESP_LOGW(TAG, "no frames for %lld s -- check the bus", (long long)idle);
      }
    } else if (warned_quiet) {
      warned_quiet = false;
      ESP_LOGI(TAG, "frames again after a quiet spell");
    }
  }
}
