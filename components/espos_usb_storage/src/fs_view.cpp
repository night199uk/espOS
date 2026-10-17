/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "fs_view.h"

#include <cstdlib>

#include "card_lock.h"
#include "diskio_impl.h"
#include "esp_log.h"

/* RealBathy's directory and file names are longer than 8.3; without long
 * names FatFs sees only their short aliases and finds nothing to follow. */
#if !CONFIG_FATFS_LFN_HEAP && !CONFIG_FATFS_LFN_STACK
#error \
    "espos_usb_storage needs FatFs long file names: set CONFIG_FATFS_LFN_HEAP=y"
#endif

namespace espos_usb_storage {

namespace {

const char* TAG = "usb_storage";

/* One card per firmware, and a pass at a time: the driver callbacks find the
 * card here because FatFs hands them only the drive number. */
sdmmc_card_t* s_ro_card = nullptr;

DSTATUS ro_init(BYTE) { return STA_PROTECT; }
DSTATUS ro_status(BYTE) { return STA_PROTECT; }

DRESULT ro_read(BYTE, BYTE* buff, uint32_t sector, unsigned count) {
  return card_lock::read(s_ro_card, buff, sector, count) == ESP_OK ? RES_OK
                                                                   : RES_ERROR;
}

DRESULT ro_write(BYTE, const BYTE*, uint32_t, unsigned) { return RES_WRPRT; }

DRESULT ro_ioctl(BYTE, BYTE cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC:
      return RES_OK;
    case GET_SECTOR_COUNT:
      *static_cast<LBA_t*>(buff) = s_ro_card->csd.capacity;
      return RES_OK;
    case GET_SECTOR_SIZE:
      *static_cast<WORD*>(buff) = s_ro_card->csd.sector_size;
      return RES_OK;
    case GET_BLOCK_SIZE:
      *static_cast<DWORD*>(buff) = 1;
      return RES_OK;
    default:
      return RES_PARERR;
  }
}

const ff_diskio_impl_t kReadOnly = {
    .init = ro_init,
    .status = ro_status,
    .read = ro_read,
    .write = ro_write,
    .ioctl = ro_ioctl,
};

}  // namespace

SnapshotFs::SnapshotFs(sdmmc_card_t* card)
    : card_(card), gen0_(card_lock::generation()) {}

SnapshotFs::~SnapshotFs() {
  close_file();
  if (registered_) f_mount(nullptr, drive_path("").c_str(), 0);
  if (pdrv_ != 0xFF) ff_diskio_unregister(pdrv_);
  free(fs_);
  s_ro_card = nullptr;
}

std::string SnapshotFs::drive_path(const std::string& rel) const {
  std::string p = std::to_string(pdrv_) + ":";
  if (!rel.empty()) p += "/" + rel;
  return p;
}

bool SnapshotFs::open() {
  if (ff_diskio_get_drive(&pdrv_) != ESP_OK) {
    pdrv_ = 0xFF;
    ESP_LOGW(TAG, "no free FatFs drive number");
    return false;
  }
  s_ro_card = card_;
  ff_diskio_register(pdrv_, &kReadOnly);
  fs_ = static_cast<FATFS*>(calloc(1, sizeof(FATFS)));
  if (!fs_) return false;
  /* Forced, so a card that is not FAT (or not formatted yet: the host may be
   * doing that right now) fails here and not on the first read. */
  /* FatFs keeps the FATFS pointer even when the mount fails, so it must be
   * unregistered in every case before fs_ is freed. */
  registered_ = true;
  return f_mount(fs_, drive_path("").c_str(), 1) == FR_OK;
}

bool SnapshotFs::list(const std::string& dir, std::vector<FileEntry>* out) {
  FF_DIR d;
  const FRESULT res = f_opendir(&d, drive_path(dir).c_str());
  /* A watched directory that does not exist yet is an empty one: the plotter
   * creates it with its first recording. */
  if (res == FR_NO_PATH || res == FR_NO_FILE) return true;
  if (res != FR_OK) return false;
  FILINFO fno;
  bool ok = true;
  for (;;) {
    const FRESULT r = f_readdir(&d, &fno);
    if (r != FR_OK) {
      ok = false;
      break;
    }
    if (fno.fname[0] == '\0') break;
    if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    out->push_back(FileEntry{dir + "/" + fno.fname, fno.fsize});
  }
  f_closedir(&d);
  return ok;
}

void SnapshotFs::close_file() {
  if (file_open_) f_close(&file_);
  file_open_ = false;
}

bool SnapshotFs::read(const std::string& path, uint64_t offset, uint8_t* buf,
                      size_t len) {
  if (!file_open_ || file_path_ != path) {
    close_file();
    if (f_open(&file_, drive_path(path).c_str(), FA_READ) != FR_OK)
      return false;
    file_open_ = true;
    file_path_ = path;
  }
  UINT got = 0;
  return f_lseek(&file_, offset) == FR_OK &&
         f_read(&file_, buf, len, &got) == FR_OK && got == len;
}

bool SnapshotFs::unchanged() { return card_lock::generation() == gen0_; }

}  // namespace espos_usb_storage
