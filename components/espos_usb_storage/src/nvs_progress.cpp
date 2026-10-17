/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "nvs_progress.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "nvs.h"

namespace espos_usb_storage {

namespace {

/* The watched directory plus a name of up to FAT's 255-character limit. */
constexpr size_t kMaxPath = 512;

/* Only as long as the path needs: a fixed-size record would rewrite a
 * mostly empty path buffer to flash on every save. */
struct Record {
  uint64_t offset;
  uint32_t head_len;
  uint32_t head_crc;
  char path[kMaxPath];
};
constexpr size_t kHeader = offsetof(Record, path);

std::string key_for(const std::string& path) {
  char key[12];
  snprintf(key, sizeof(key), "p%08lx",
           static_cast<unsigned long>(crc32(
               0, reinterpret_cast<const uint8_t*>(path.data()), path.size())));
  return key;
}

bool get(nvs_handle_t h, const char* key, Record* rec, std::string* path) {
  size_t len = sizeof(*rec);
  if (nvs_get_blob(h, key, rec, &len) != ESP_OK || len < kHeader) return false;
  path->assign(rec->path, len - kHeader);
  return true;
}

}  // namespace

bool NvsProgressStore::load(const std::string& path, Progress* out) {
  nvs_handle_t h;
  if (nvs_open(ns_, NVS_READONLY, &h) != ESP_OK) return false;
  Record rec;
  std::string stored;
  const bool ok =
      get(h, key_for(path).c_str(), &rec, &stored) && stored == path;
  nvs_close(h);
  if (ok) *out = Progress{rec.offset, rec.head_len, rec.head_crc};
  return ok;
}

bool NvsProgressStore::save(const std::string& path, const Progress& p) {
  if (path.size() > kMaxPath) return false;
  Record rec;
  rec.offset = p.offset;
  rec.head_len = p.head_len;
  rec.head_crc = p.head_crc;
  memcpy(rec.path, path.data(), path.size());
  nvs_handle_t h;
  if (nvs_open(ns_, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, key_for(path).c_str(), &rec,
                               kHeader + path.size()) == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

void NvsProgressStore::erase(const std::string& path) {
  nvs_handle_t h;
  if (nvs_open(ns_, NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_erase_key(h, key_for(path).c_str()) == ESP_OK) nvs_commit(h);
  nvs_close(h);
}

std::vector<std::string> NvsProgressStore::paths() {
  std::vector<std::string> out;
  nvs_iterator_t it = nullptr;
  esp_err_t err =
      nvs_entry_find(NVS_DEFAULT_PART_NAME, ns_, NVS_TYPE_BLOB, &it);
  nvs_handle_t h;
  if (err != ESP_OK || nvs_open(ns_, NVS_READONLY, &h) != ESP_OK) {
    nvs_release_iterator(it);
    return out;
  }
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    Record rec;
    std::string path;
    if (get(h, info.key, &rec, &path)) out.push_back(path);
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  nvs_close(h);
  return out;
}

}  // namespace espos_usb_storage
