/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_USB_STORAGE_NVS_PROGRESS_H_
#define ESPOS_USB_STORAGE_NVS_PROGRESS_H_

#include <string>
#include <vector>

#include "espos_usb_storage/follower.h"

namespace espos_usb_storage {

/// Progress in NVS, never on the card: the card belongs to the host.
///
/// NVS keys are 15 characters, so a file is keyed by the CRC32 of its path
/// and the path is stored in the record to catch a collision. Two colliding
/// paths would keep evicting each other's progress and be resent each pass;
/// with a few dozen recordings the odds are around one in ten million.
class NvsProgressStore : public ProgressStore {
 public:
  explicit NvsProgressStore(const char* ns) : ns_(ns) {}
  bool load(const std::string& path, Progress* out) override;
  bool save(const std::string& path, const Progress& p) override;
  void erase(const std::string& path) override;
  std::vector<std::string> paths() override;

 private:
  const char* ns_;
};

}  // namespace espos_usb_storage

#endif  // ESPOS_USB_STORAGE_NVS_PROGRESS_H_
