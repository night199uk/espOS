/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_N2K_MULTI_NODE_H_
#define ESPOS_N2K_MULTI_NODE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "espos_n2k/can_frame.h"
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/twai_transmitter.h"

#include "N2kMsg.h"

class tNMEA2000;

namespace espos_n2k {

/// One logical N2K device in a MultiNode. The fields are the NMEA 2000
/// identity and product information that make an MFD show it as a separate
/// device; the source address is the node's preferred base address plus the
/// device index.
struct DeviceConfig {
  /// NAME: device class and function from the NMEA 2000 lists. Required.
  uint8_t device_class = 0;
  uint8_t device_function = 0;
  /// 2046 is the uncertified-device manufacturer code used by the NMEA2000
  /// library's examples.
  uint16_t manufacturer_code = 2046;
  /// 4 is Marine.
  uint8_t industry_group = 4;
  /// 21 bits. 0 derives it from the base MAC and the device index.
  uint32_t unique_number = 0;
  /// The 8-bit NMEA 2000 device instance; the library splits it into the
  /// NAME's lower and upper fields.
  uint8_t device_instance = 0;
  uint8_t system_instance = 0;

  /// Product information (PGN 126996). model_id is required.
  const char* model_id = nullptr;
  const char* model_version = "";
  /// Product serial code. nullptr derives one from the base MAC and index.
  const char* serial_code = nullptr;
  uint16_t product_code = 0;
  uint8_t load_equivalency = 1;

  /// Zero-terminated PGN lists in static storage; the library keeps the
  /// pointers.
  const unsigned long* transmit_pgns = nullptr;
  const unsigned long* receive_pgns = nullptr;
};

struct MultiNodeConfig {
  const DeviceConfig* devices = nullptr;
  uint8_t device_count = 0;
  /// Preferred source address of device 0; device i uses address + i.
  uint8_t preferred_address = 34;
};

/// Several logical NMEA 2000 devices on one physical bus and task.
///
/// This is the multi-device counterpart of Node. It is for a device that has
/// to present several identities -- one per tank, say -- while sharing one
/// TWAI receiver/transmitter. Each device gets its own NAME, address claim,
/// product information, instances and PGN lists; send() selects which one
/// speaks.
///
/// Unlike Node, it does not persist addresses or expose the N2K settings
/// page: the identities are deliberately fixed by the firmware. The stock
/// NMEA2000 library has no per-device SetConfigurationInformation(), so
/// installation descriptions are shared/left at the library default.
///
/// Build with CONFIG_ESPOS_N2K_NODE. Make it static: the task and the graph
/// nodes may hold its address for the life of the firmware.
class MultiNode {
 public:
  static constexpr uint8_t kMaxDevices = 9;

  /// Both must outlive the node. Start the receiver and transmitter first.
  MultiNode(TwaiReceiver* receiver, TwaiTransmitter* transmitter,
            const MultiNodeConfig& config);
  ~MultiNode();
  MultiNode(const MultiNode&) = delete;
  MultiNode& operator=(const MultiNode&) = delete;

  /// Configure all devices and start the node task. The receiver and
  /// transmitter must already be started. Call after espos_start().
  esp_err_t start();

  /// Queue a message for one logical device. Non-blocking; false when the
  /// node is not started, the index is invalid, or the queue is full.
  bool send(uint8_t device_index, const tN2kMsg& message);

  uint8_t device_count() const { return device_count_; }
  /// True once the bus is open. Individual devices may still be claiming;
  /// send() queues until their claim settles.
  bool on_bus() const { return on_bus_.load(std::memory_order_relaxed); }
  /// Current source address of a device, or 254 while it has none.
  uint8_t address(uint8_t device_index) const;

  /// The library itself, for what this class does not wrap. Valid after
  /// start(); do not use it from the node task or another task while it is
  /// running.
  tNMEA2000& library();

 private:
  class Bus;

  static void task_entry(void* arg);
  void run();
  void cleanup();

  TwaiReceiver* receiver_;
  TwaiTransmitter* transmitter_;
  MultiNodeConfig config_;
  Bus* bus_ = nullptr;
  QueueHandle_t frames_ = nullptr;
  QueueHandle_t outbox_ = nullptr;
  TaskHandle_t task_ = nullptr;
  TwaiReceiver::ListenerId rx_listener_ = TwaiReceiver::kNoListener;
  std::atomic<bool> on_bus_{false};
  std::atomic<uint8_t> addresses_[kMaxDevices] = {};
  uint8_t device_count_ = 0;
  bool started_ = false;
};

}  // namespace espos_n2k

#endif  // ESPOS_N2K_MULTI_NODE_H_
