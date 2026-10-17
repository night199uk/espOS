// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::sensors::system — the device reporting on itself.
//
// espos_sk ALREADY publishes the standard set (uptime, free heap, RSSI, …)
// under `espos.<label>.*` on its own schedule. These nodes are for the case
// that set does not cover: putting one of those numbers on a path of YOUR
// choosing, feeding it through a transform, or alarming on it in the graph
// rather than in espos_health.
//
// Do not wire these to `espos.<label>.freeHeap` -- that path is already
// published, and two writers on one path is a fight nobody wins.
//
//   FreeHeap heap("heap", 10000);
//   heap >> below_20k >> notify;    // a graph-side alarm, not a duplicate
//   publish
#pragma once

#include <cstdint>
#include <string>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "espos_flow/flow.hpp"

namespace espos::sensors::system {

// Seconds since boot. int32_t rather than float: at 1 Hz a float loses
// integer precision after ~97 days of uptime, which is exactly the uptime
// worth reporting.
inline int32_t uptime_s() {
  return static_cast<int32_t>(esp_timer_get_time() / 1000000);
}

// Free heap in bytes, all capabilities.
inline int32_t free_heap() {
  return static_cast<int32_t>(esp_get_free_heap_size());
}

// Free INTERNAL heap. On a chip with PSRAM this is the number that matters:
// ~30 MB of free SPIRAM hides an internal pool that is nearly gone, and it is
// internal RAM that a WiFi buffer or a task stack has to come from. A device
// that has been up for days and stops accepting connections is almost always
// this, not total heap. 8BIT too: a classic ESP32's spare IRAM is internal
// heap that only 32-bit accesses may use, so it holds none of those.
inline int32_t free_internal() {
  return static_cast<int32_t>(
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// The largest single block of internal RAM. Fragmentation shows up here
// first: plenty free, none of it contiguous, and the next allocation fails.
inline int32_t largest_internal_block() {
  return static_cast<int32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// Why the device last restarted, as the short string a human reads in a
// dashboard. A panic or a task watchdog here is the first thing to look at
// after a device "just rebooted".
inline const char* reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interruptWatchdog";
    case ESP_RST_TASK_WDT:
      return "taskWatchdog";
    case ESP_RST_WDT:
      return "otherWatchdog";
    case ESP_RST_DEEPSLEEP:
      return "deepSleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

// The nodes. Each is a Poll over the function above, so they cost a timer
// and nothing else. The function pointer is the template argument, which is
// why these are `using` rather than classes: no std::function, no capture.
using Uptime = espos::flow::Poll<int32_t, int32_t (*)()>;
using FreeHeap = espos::flow::Poll<int32_t, int32_t (*)()>;
using InternalFreeHeap = espos::flow::Poll<int32_t, int32_t (*)()>;
using LargestInternalBlock = espos::flow::Poll<int32_t, int32_t (*)()>;

// Convenience makers, so the wiring reads as what it is:
//
//   auto& up = make_uptime(g, "uptime", 10000);
template <typename Graph>
Uptime& make_uptime(Graph& g, const char* id, uint32_t period_ms) {
  return g.template make<Uptime>(id, period_ms, &uptime_s);
}
template <typename Graph>
FreeHeap& make_free_heap(Graph& g, const char* id, uint32_t period_ms) {
  return g.template make<FreeHeap>(id, period_ms, &free_heap);
}
template <typename Graph>
InternalFreeHeap& make_internal_free_heap(Graph& g, const char* id,
                                          uint32_t period_ms) {
  return g.template make<InternalFreeHeap>(id, period_ms, &free_internal);
}

// ResetReason is emitted ONCE, at start: it cannot change while the device
// is running, and a device that republishes it every minute is describing a
// reboot that happened days ago as if it were news.
class ResetReason : public espos::flow::NodeBase,
                    public espos::flow::Producer<std::string> {
 public:
  explicit ResetReason(const char* id) : NodeBase(id) {}

  // Call from the wiring function, after the graph is connected.
  void emit_now() { this->emit(std::string(reset_reason())); }

 protected:
  const char* node_id_for_error() const override { return id(); }
};

}  // namespace espos::sensors::system
