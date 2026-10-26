/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "espos_n2k/multi_node.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "NMEA2000.h"
#include "node_identity.h"

namespace espos_n2k {

namespace {

constexpr const char* kTag = "n2k_multi";
constexpr TickType_t kLoopTicks = pdMS_TO_TICKS(10);
constexpr TickType_t kOpenWarnTicks = pdMS_TO_TICKS(5000);

// tN2kMsg has a virtual member, so copy only the fields a send needs through
// the FreeRTOS queue.
struct Outgoing {
  uint8_t device_index;
  uint32_t pgn;
  uint8_t priority;
  uint8_t destination;
  uint8_t len;
  uint8_t data[tN2kMsg::MaxDataLen];
};

}  // namespace

// tNMEA2000 over the shared espOS TWAI receiver/transmitter. The library
// calls these from ParseMessages() and SendMsg(), both on the node task.
class MultiNode::Bus : public tNMEA2000 {
 public:
  Bus(TwaiReceiver* rx, TwaiTransmitter* tx) : rx_(rx), tx_(tx) {}
  virtual ~Bus() = default;

  void set_frames(QueueHandle_t queue, TwaiReceiver::ListenerId own) {
    frames_ = queue;
    own_ = own;
  }

 protected:
  bool CANOpen() override { return rx_->bus_running(); }

  bool CANSendFrame(unsigned long id, unsigned char len,
                    const unsigned char* buf, bool) override {
    CanMessage message;
    message.frame.id = id;
    message.frame.extended = true;
    message.frame.dlc = len > kCanMaxData ? kCanMaxData : len;
    std::memcpy(message.frame.data, buf, message.frame.dlc);

    if (!tx_->transmit(message)) return false;

    message.timestamp_us = esp_timer_get_time();
    rx_->loopback(message, own_);
    return true;
  }

  bool CANGetFrame(unsigned long& id, unsigned char& len,
                   unsigned char* buf) override {
    CanFrame frame;
    if (!frames_ || xQueueReceive(frames_, &frame, 0) != pdTRUE) return false;
    id = frame.id;
    len = frame.dlc > kCanMaxData ? kCanMaxData : frame.dlc;
    std::memcpy(buf, frame.data, len);
    return true;
  }

 private:
  TwaiReceiver* rx_;
  TwaiTransmitter* tx_;
  QueueHandle_t frames_ = nullptr;
  TwaiReceiver::ListenerId own_ = TwaiReceiver::kNoListener;
};

MultiNode::MultiNode(TwaiReceiver* receiver, TwaiTransmitter* transmitter,
                     const MultiNodeConfig& config)
    : receiver_(receiver), transmitter_(transmitter), config_(config) {
  for (auto& address : addresses_) address.store(detail::kNullAddress);
}

MultiNode::~MultiNode() {
  if (!started_) cleanup();
}

void MultiNode::cleanup() {
  if (rx_listener_ != TwaiReceiver::kNoListener) {
    receiver_->remove_listener(rx_listener_);
    rx_listener_ = TwaiReceiver::kNoListener;
  }
  if (bus_ != nullptr) {
    bus_->set_frames(nullptr, TwaiReceiver::kNoListener);
  }
  if (frames_ != nullptr) {
    vQueueDelete(frames_);
    frames_ = nullptr;
  }
  if (outbox_ != nullptr) {
    vQueueDelete(outbox_);
    outbox_ = nullptr;
  }
  delete bus_;
  bus_ = nullptr;
  device_count_ = 0;
  on_bus_.store(false);
  started_ = false;
}

esp_err_t MultiNode::start() {
  if (started_) return ESP_ERR_INVALID_STATE;
  if (receiver_ == nullptr || transmitter_ == nullptr ||
      config_.devices == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (config_.device_count == 0 || config_.device_count > kMaxDevices) {
    return ESP_ERR_INVALID_ARG;
  }
  if (static_cast<unsigned>(config_.preferred_address) + config_.device_count -
          1 >
      N2kMaxCanBusAddress) {
    ESP_LOGE(kTag, "preferred address range %u..%u is not claimable",
             static_cast<unsigned>(config_.preferred_address),
             static_cast<unsigned>(config_.preferred_address +
                                   config_.device_count - 1));
    return ESP_ERR_INVALID_ARG;
  }

  bus_ = new Bus(receiver_, transmitter_);
  if (bus_ == nullptr) return ESP_ERR_NO_MEM;
  device_count_ = config_.device_count;

  // SetDeviceCount must be first, before any other tNMEA2000 call.
  bus_->SetDeviceCount(device_count_);
  bus_->SetMode(tNMEA2000::N2km_NodeOnly, config_.preferred_address);
  bus_->SetN2kCANMsgBufSize(CONFIG_ESPOS_N2K_NODE_FAST_PACKETS);

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_BASE);
  char mac_serial[13];
  detail::serial_from_mac(mac, mac_serial, sizeof(mac_serial));

  for (uint8_t i = 0; i < device_count_; i++) {
    const DeviceConfig& device = config_.devices[i];
    if (device.model_id == nullptr || device.device_class == 0 ||
        device.device_function == 0) {
      ESP_LOGE(kTag, "device %u needs model_id, class and function",
               static_cast<unsigned>(i));
      cleanup();
      return ESP_ERR_INVALID_ARG;
    }

    const uint8_t source = bus_->GetN2kSource(i);
    addresses_[i].store(source);
    const uint32_t unique =
        (device.unique_number != 0 ? device.unique_number
                                   : detail::unique_number_from_mac(mac) + i) &
        detail::kUniqueNumberMask;

    char serial[33];
    if (device.serial_code != nullptr && *device.serial_code != '\0') {
      std::snprintf(serial, sizeof(serial), "%s", device.serial_code);
    } else {
      std::snprintf(serial, sizeof(serial), "%s-%u", mac_serial,
                    static_cast<unsigned>(i));
    }

    bus_->SetProductInformation(
        serial, device.product_code, device.model_id,
        esp_app_get_description()->version,
        device.model_version != nullptr ? device.model_version : "",
        device.load_equivalency, 0xffff, 0xff, i);
    bus_->SetDeviceInformation(unique, device.device_function,
                               device.device_class, device.manufacturer_code,
                               device.industry_group, i);
    bus_->SetDeviceInformationInstances(
        detail::device_instance_lower(device.device_instance),
        detail::device_instance_upper(device.device_instance),
        device.system_instance, i);
    if (device.transmit_pgns != nullptr) {
      bus_->ExtendTransmitMessages(device.transmit_pgns, i);
    }
    if (device.receive_pgns != nullptr) {
      bus_->ExtendReceiveMessages(device.receive_pgns, i);
    }
  }
  bus_->EnableForward(false);

  frames_ = xQueueCreate(CONFIG_ESPOS_N2K_RX_QUEUE_DEPTH, sizeof(CanFrame));
  outbox_ = xQueueCreate(CONFIG_ESPOS_N2K_NODE_TX_QUEUE, sizeof(Outgoing));
  if (frames_ == nullptr || outbox_ == nullptr) {
    cleanup();
    return ESP_ERR_NO_MEM;
  }

  rx_listener_ = receiver_->add_listener([this](const CanMessage& message) {
    if (!message.frame.extended || message.frame.remote) return;
    if (xQueueSend(frames_, &message.frame, 0) != pdTRUE) {
      // The receiver task must not block; drop the frame.
    }
  });
  if (rx_listener_ == TwaiReceiver::kNoListener) {
    cleanup();
    return ESP_ERR_NO_MEM;
  }
  bus_->set_frames(frames_, rx_listener_);

  if (xTaskCreate(&MultiNode::task_entry, "n2k_multi",
                  CONFIG_ESPOS_N2K_NODE_TASK_STACK, this, 4,
                  &task_) != pdPASS) {
    cleanup();
    return ESP_ERR_NO_MEM;
  }

  started_ = true;
  ESP_LOGI(
      kTag, "%u devices at source %u..%u", static_cast<unsigned>(device_count_),
      static_cast<unsigned>(config_.preferred_address),
      static_cast<unsigned>(config_.preferred_address + device_count_ - 1));
  return ESP_OK;
}

bool MultiNode::send(uint8_t device_index, const tN2kMsg& message) {
  if (!started_ || outbox_ == nullptr) return false;
  if (device_index >= device_count_) return false;
  if (message.DataLen < 0 || message.DataLen > tN2kMsg::MaxDataLen) {
    return false;
  }

  Outgoing outgoing;
  outgoing.device_index = device_index;
  outgoing.pgn = message.PGN;
  outgoing.priority = message.Priority;
  outgoing.destination = message.Destination;
  outgoing.len = static_cast<uint8_t>(message.DataLen);
  std::memcpy(outgoing.data, message.Data, outgoing.len);
  return xQueueSend(outbox_, &outgoing, 0) == pdTRUE;
}

void MultiNode::task_entry(void* arg) {
  static_cast<MultiNode*>(arg)->run();
  vTaskDelete(nullptr);
}

void MultiNode::run() {
  const TickType_t began = xTaskGetTickCount();
  bool opened = false;
  bool warned = false;
  Outgoing outgoing;

  for (;;) {
    bus_->ParseMessages();
    if (!bus_->IsOpen()) {
      if (!warned && xTaskGetTickCount() - began > kOpenWarnTicks) {
        warned = true;
        ESP_LOGE(kTag,
                 "the CAN bus is not up -- start the receiver before the "
                 "multi-node");
      }
      vTaskDelay(kLoopTicks);
      continue;
    }

    if (!opened) {
      opened = true;
      xQueueReset(frames_);
      on_bus_.store(true);
      for (uint8_t i = 0; i < device_count_; i++) {
        const uint8_t source = bus_->GetN2kSource(i);
        addresses_[i].store(source);
        ESP_LOGI(kTag, "device %u source %u", static_cast<unsigned>(i),
                 static_cast<unsigned>(source));
      }
    }

    for (uint8_t i = 0; i < device_count_; i++) {
      addresses_[i].store(bus_->GetN2kSource(i));
    }

    while (xQueuePeek(outbox_, &outgoing, 0) == pdTRUE) {
      tN2kMsg message;
      message.Clear();
      message.SetPGN(outgoing.pgn);
      message.Priority = outgoing.priority;
      message.Destination = outgoing.destination;
      message.DataLen = outgoing.len;
      std::memcpy(message.Data, outgoing.data, outgoing.len);

      if (!bus_->SendMsg(message, outgoing.device_index)) break;
      xQueueReceive(outbox_, &outgoing, 0);
    }

    vTaskDelay(kLoopTicks);
  }
}

uint8_t MultiNode::address(uint8_t device_index) const {
  if (device_index >= kMaxDevices) return detail::kNullAddress;
  return addresses_[device_index].load(std::memory_order_relaxed);
}

tNMEA2000& MultiNode::library() { return *bus_; }

}  // namespace espos_n2k
