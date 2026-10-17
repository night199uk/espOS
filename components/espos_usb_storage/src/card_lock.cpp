/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "card_lock.h"

#include <atomic>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

namespace espos_usb_storage::card_lock {

namespace {

std::atomic<sdmmc_card_t*> s_card{nullptr};
std::atomic<uint32_t> s_gen{0};
std::atomic<int64_t> s_last_write_us{0};
StaticSemaphore_t s_mutex_buf;
SemaphoreHandle_t s_mutex = nullptr;

}  // namespace

void track(sdmmc_card_t* card) {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
  s_card.store(card);
}

uint32_t generation() { return s_gen.load(); }
int64_t last_write_us() { return s_last_write_us.load(); }

esp_err_t read(sdmmc_card_t* card, void* dst, size_t start, size_t count) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  const esp_err_t err = sdmmc_read_sectors(card, dst, start, count);
  xSemaphoreGive(s_mutex);
  return err;
}

}  // namespace espos_usb_storage::card_lock

using espos_usb_storage::card_lock::s_card;
using espos_usb_storage::card_lock::s_gen;
using espos_usb_storage::card_lock::s_last_write_us;
using espos_usb_storage::card_lock::s_mutex;

extern "C" esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card,
                                                const void* src,
                                                size_t start_sector,
                                                size_t sector_count);

/* Every sdmmc_write_sectors() in the firmware lands here (CMakeLists.txt
 * wraps it). Writes to any other card pass straight through. */
extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card,
                                                const void* src,
                                                size_t start_sector,
                                                size_t sector_count) {
  if (card != s_card.load()) {
    return __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  /* Before the write, so a reader comparing generations after its own read
   * cannot miss a write that overlapped it. */
  s_gen.fetch_add(1);
  const esp_err_t err =
      __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
  s_last_write_us.store(esp_timer_get_time());
  xSemaphoreGive(s_mutex);
  return err;
}
