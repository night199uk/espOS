/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "espos_usb_storage/usb_storage.h"

#include <cstdlib>
#include <memory>
#include <utility>

#include "card_lock.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs_view.h"
#include "nvs_progress.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"

namespace espos_usb_storage {

namespace {

const char* TAG = "usb_storage";

sdmmc_card_t* s_card = nullptr;

esp_err_t card_init(const SdConfig& sd, sdmmc_card_t** out) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = sd.slot;
  host.max_freq_khz = sd.freq_khz;
  if (sd.ldo_chan >= 0) {
#if SOC_GP_LDO_SUPPORTED
    sd_pwr_ctrl_ldo_config_t ldo = {};
    ldo.ldo_chan_id = sd.ldo_chan;
    sd_pwr_ctrl_handle_t pwr = nullptr;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr), TAG,
                        "SD power LDO %d", sd.ldo_chan);
    host.pwr_ctrl_handle = pwr;
#else
    ESP_LOGE(TAG,
             "this chip has no on-chip LDO for the SD rail; set ldo_chan = -1");
    return ESP_ERR_NOT_SUPPORTED;
#endif
  }

  /* Every failure below releases the LDO channel, or a start() retried after
   * the card is inserted would find it still taken. */
  auto release_ldo = [&host]() {
#if SOC_GP_LDO_SUPPORTED
    if (host.pwr_ctrl_handle) sd_pwr_ctrl_del_on_chip_ldo(host.pwr_ctrl_handle);
    host.pwr_ctrl_handle = nullptr;
#endif
  };

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  const bool four_bit = sd.d1 >= 0 && sd.d2 >= 0 && sd.d3 >= 0;
  slot.width = four_bit ? 4 : 1;
#if SOC_SDMMC_USE_GPIO_MATRIX
  slot.clk = static_cast<gpio_num_t>(sd.clk);
  slot.cmd = static_cast<gpio_num_t>(sd.cmd);
  slot.d0 = static_cast<gpio_num_t>(sd.d0);
  if (four_bit) {
    slot.d1 = static_cast<gpio_num_t>(sd.d1);
    slot.d2 = static_cast<gpio_num_t>(sd.d2);
    slot.d3 = static_cast<gpio_num_t>(sd.d3);
  }
#endif
  /* Most boards rely on 10k pull-ups on the card lines; the internal ones are
   * weak but keep a board without them from reading floating lines. */
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  /* The P4's SDMMC controller has two slots, and on the Waveshare P4 boards
   * slot 1 is the SDIO link to the C6 radio, which esp_hosted brought up
   * first. The controller is then already running: that is not an error. */
  const esp_err_t host_err = host.init();
  if (host_err != ESP_OK && host_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "SDMMC host: %s", esp_err_to_name(host_err));
    release_ldo();
    return host_err;
  }
  const esp_err_t slot_err = sdmmc_host_init_slot(host.slot, &slot);
  if (slot_err != ESP_OK) {
    ESP_LOGE(TAG, "SDMMC slot: %s", esp_err_to_name(slot_err));
    release_ldo();
    return slot_err;
  }

  auto* card = static_cast<sdmmc_card_t*>(calloc(1, sizeof(sdmmc_card_t)));
  const esp_err_t err = card ? sdmmc_card_init(&host, card) : ESP_ERR_NO_MEM;
  if (err != ESP_OK) {
    free(card);
    release_ldo();
    ESP_LOGE(TAG, "no card, or it does not answer: %s", esp_err_to_name(err));
    return err;
  }
  *out = card;
  return ESP_OK;
}

void msc_event(tinyusb_msc_storage_handle_t, tinyusb_msc_event_t* ev, void*) {
  ESP_LOGI(TAG, "MSC event %d (mount point %d)", ev->id, ev->mount_point);
}

}  // namespace

esp_err_t start(const Config& cfg) {
  if (s_card) return ESP_ERR_INVALID_STATE;

  tinyusb_config_t tusb = TINYUSB_CONFIG_FULL_SPEED(nullptr, nullptr);
#if SOC_USB_OTG_PERIPH_NUM > 1
  if (cfg.port != UsbPort::kFullSpeed) tusb = TINYUSB_DEFAULT_CONFIG();
#else
  if (cfg.port == UsbPort::kHighSpeed) {
    ESP_LOGE(TAG, "this chip has no high-speed USB port");
    return ESP_ERR_NOT_SUPPORTED;
  }
#endif

  sdmmc_card_t* card = nullptr;
  ESP_RETURN_ON_ERROR(card_init(cfg.sd, &card), TAG, "card");
  sdmmc_card_print_info(stdout, card);
  card_lock::track(card);

  /* Unwound on failure so start() can be called again; the usual causes are
   * configuration (CONFIG_TINYUSB_MSC_ENABLED) or memory. */
  bool driver = false;
  tinyusb_msc_storage_handle_t storage = nullptr;
  auto fail = [&](esp_err_t err, const char* what) {
    ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    if (storage) tinyusb_msc_delete_storage(storage);
    if (driver) tinyusb_msc_uninstall_driver();
    card_lock::track(nullptr);
#if SOC_GP_LDO_SUPPORTED
    if (card->host.pwr_ctrl_handle)
      sd_pwr_ctrl_del_on_chip_ldo(card->host.pwr_ctrl_handle);
#endif
    free(card);
    return err;
  };

  /* auto_mount_off: by default esp_tinyusb mounts the card read-write for the
   * application whenever the host goes away, and formats it if it finds no
   * filesystem. Either would put two writers on one FAT the moment the host
   * comes back; the card stays the host's, always. */
  tinyusb_msc_driver_config_t drv = {};
  drv.user_flags.auto_mount_off = 1;
  drv.callback = msc_event;
  esp_err_t err = tinyusb_msc_install_driver(&drv);
  if (err != ESP_OK)
    return fail(err, "MSC driver (is CONFIG_TINYUSB_MSC_ENABLED set?)");
  driver = true;

  tinyusb_msc_storage_config_t sc = {};
  sc.medium.card = card;
  sc.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
  sc.fat_fs.do_not_format = true;
  err = tinyusb_msc_new_storage_sdmmc(&sc, &storage);
  if (err != ESP_OK) return fail(err, "MSC storage");

  err = tinyusb_driver_install(&tusb);
  if (err != ESP_OK) return fail(err, "TinyUSB");
  s_card = card;
  ESP_LOGI(TAG, "card exported over USB (%s), %llu MB",
           tusb.port == TINYUSB_PORT_FULL_SPEED_0 ? "full speed" : "high speed",
           static_cast<unsigned long long>(
               static_cast<uint64_t>(card->csd.capacity) *
               card->csd.sector_size / (1024 * 1024)));
  return ESP_OK;
}

uint32_t write_generation() { return card_lock::generation(); }
int64_t last_write_us() { return card_lock::last_write_us(); }
bool host_connected() { return tud_mounted(); }

namespace {

struct FollowTask {
  FollowConfig cfg;
  NvsProgressStore store;
  Follower follower;

  explicit FollowTask(FollowConfig c)
      : cfg(std::move(c)),
        store(cfg.nvs_namespace),
        follower(cfg.follower, &store) {}
};

bool host_quiet(uint32_t quiet_ms) {
  const int64_t last = card_lock::last_write_us();
  return last == 0 || esp_timer_get_time() - last >= int64_t{quiet_ms} * 1000;
}

void follow_task(void* arg) {
  auto* t = static_cast<FollowTask*>(arg);
  bool volume_ok = true;
  for (;;) {
    /* The view takes its generation baseline before the quiet check: a write
     * landing between the two would otherwise count as "before". On the heap,
     * because its FIL carries a sector buffer of up to 4 KB. */
    auto fs = std::make_unique<SnapshotFs>(s_card);
    if (host_quiet(t->cfg.quiet_ms)) {
      const bool opened = fs->open();
      if (opened != volume_ok) {
        if (opened) {
          ESP_LOGI(TAG, "FAT volume readable");
        } else {
          ESP_LOGW(TAG, "no readable FAT volume on the card (FAT32 needed)");
        }
        volume_ok = opened;
      }
      if (opened) {
        const PassResult r = t->follower.run_pass(
            *fs, t->cfg.sink, esp_timer_get_time() / 1000, fs->generation());
        if (r.chunks)
          ESP_LOGI(TAG, "handed off %u chunk(s), %llu bytes",
                   static_cast<unsigned>(r.chunks),
                   static_cast<unsigned long long>(r.bytes));
        if (r.interrupted) ESP_LOGD(TAG, "pass interrupted by host writes");
        if (r.store_failed) ESP_LOGW(TAG, "could not save progress to NVS");
      }
    }
    fs.reset();
    vTaskDelay(pdMS_TO_TICKS(t->cfg.poll_ms));
  }
}

}  // namespace

esp_err_t follow(FollowConfig cfg) {
  /* One follower per card: the read-only FAT view's disk driver is global,
   * and two tasks reading one exported card gain nothing. */
  static bool s_following = false;
  if (!s_card || s_following) return ESP_ERR_INVALID_STATE;
  if (!cfg.sink) return ESP_ERR_INVALID_ARG;
  auto* t = new FollowTask(std::move(cfg));
  /* Stack: the FAT view's FATFS object is on the heap, but FatFs path
   * handling and the sink (which typically does an HTTP request) are not
   * small. */
  if (xTaskCreate(follow_task, "usbst_follow", 8192, t, 3, nullptr) != pdPASS) {
    delete t;
    return ESP_ERR_NO_MEM;
  }
  s_following = true;
  return ESP_OK;
}

}  // namespace espos_usb_storage
