/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_USB_STORAGE_FS_VIEW_H_
#define ESPOS_USB_STORAGE_FS_VIEW_H_

#include <cstdint>
#include <string>

#include "espos_usb_storage/follower.h"
#include "ff.h"
#include "sd_protocol_types.h"

namespace espos_usb_storage {

/// A read-only FAT view of a card the USB host owns, valid for one pass.
///
/// Mounted fresh every pass so nothing cached from an earlier pass survives
/// a host write, behind a disk driver that refuses writes, so nothing here
/// can change the card even by accident.
class SnapshotFs : public FsView {
 public:
  explicit SnapshotFs(sdmmc_card_t* card);
  ~SnapshotFs() override;
  SnapshotFs(const SnapshotFs&) = delete;
  SnapshotFs& operator=(const SnapshotFs&) = delete;

  bool open();
  bool list(const std::string& dir, std::vector<FileEntry>* out) override;
  bool read(const std::string& path, uint64_t offset, uint8_t* buf,
            size_t len) override;
  bool unchanged() override;
  /// The card's write generation when this view was made.
  uint32_t generation() const { return gen0_; }

 private:
  std::string drive_path(const std::string& rel) const;

  void close_file();

  sdmmc_card_t* card_;
  uint32_t gen0_;
  /* Kept open across a file's chunks: reopening means f_lseek walking the
   * cluster chain from the start again for every chunk. */
  FIL file_;
  std::string file_path_;
  bool file_open_ = false;
  uint8_t pdrv_ = 0xFF;
  FATFS* fs_ = nullptr;
  bool registered_ = false;
};

}  // namespace espos_usb_storage

#endif  // ESPOS_USB_STORAGE_FS_VIEW_H_
