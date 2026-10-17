/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "espos_usb_storage/follower.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace espos_usb_storage {

namespace {

/* Enough of a file's head to tell two recordings apart: a RealBathy track
 * starts with a timestamped record, so a recreated file differs here. */
constexpr uint32_t kHeadLen = 32;

}  // namespace

uint64_t settled_end(uint64_t size_now, uint64_t seen_size, int64_t seen_at_ms,
                     int64_t now_ms, uint32_t settle_ms) {
  if (size_now != seen_size) return 0;
  if (now_ms - seen_at_ms < static_cast<int64_t>(settle_ms)) return 0;
  return size_now;
}

uint64_t chunk_end(uint64_t offset, uint64_t limit, uint32_t record_size,
                   size_t max_chunk) {
  if (record_size == 0) record_size = 1;
  if (limit <= offset) return offset;
  const uint64_t avail = (limit - offset) / record_size * record_size;
  uint64_t cap = static_cast<uint64_t>(max_chunk) / record_size * record_size;
  if (cap == 0) cap = record_size;
  return offset + std::min(avail, cap);
}

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool suffix_matches(const std::string& name, const std::string& suffix) {
  if (suffix.size() > name.size()) return false;
  const size_t base = name.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(name[base + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i])))
      return false;
  }
  return true;
}

Follower::Follower(FollowerConfig cfg, ProgressStore* store)
    : cfg_(std::move(cfg)), store_(store) {}

PassResult Follower::run_pass(FsView& fs, const ChunkSink& sink, int64_t now_ms,
                              uint32_t write_gen) {
  PassResult r;
  bool pending = false;
  std::set<std::string> present;

  for (const Watch& w : cfg_.watches) {
    std::vector<FileEntry> entries;
    if (!fs.list(w.dir, &entries) || !fs.unchanged()) {
      r.interrupted = true;
      return r;
    }
    /* Oldest name first is not guaranteed by FAT; sorting makes the order
     * the sink sees the same on every pass, which is what a server
     * deduplicating by (path, offset) relies on to stay simple. */
    std::sort(
        entries.begin(), entries.end(),
        [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    for (const FileEntry& e : entries) {
      if (!suffix_matches(e.path, w.suffix)) continue;
      present.insert(e.path);
      if (!follow_file(fs, sink, e, w.record_size, now_ms, write_gen, &r,
                       &pending))
        return r;
    }
  }

  /* Only a complete, undisturbed listing may prune: a file missing from a
   * listing the host was rewriting is not a deleted file. */
  if (!fs.unchanged()) {
    r.interrupted = true;
    return r;
  }
  for (const std::string& p : store_->paths()) {
    if (!present.count(p)) store_->erase(p);
  }
  for (auto it = seen_.begin(); it != seen_.end();) {
    it = present.count(it->first) ? std::next(it) : seen_.erase(it);
  }
  r.complete = !pending;
  return r;
}

bool Follower::follow_file(FsView& fs, const ChunkSink& sink,
                           const FileEntry& e, uint32_t record_size,
                           int64_t now_ms, uint32_t write_gen, PassResult* r,
                           bool* pending) {
  auto it = seen_.find(e.path);
  if (it == seen_.end()) {
    it = seen_.emplace(e.path, Seen{e.size, now_ms, write_gen, 0}).first;
  } else if (it->second.size != e.size || it->second.gen != write_gen) {
    const uint64_t stable = e.size >= it->second.stable ? it->second.stable : 0;
    it->second = Seen{e.size, now_ms, write_gen, stable};
  }
  Seen& seen = it->second;

  Progress p;
  const bool had = store_->load(e.path, &p);
  bool restarted = false;
  if (had) {
    /* A head_len beyond the fingerprint is a record this code did not write;
     * restarting is safe, reading that many bytes into head[] is not. */
    bool same =
        p.offset <= e.size && p.head_len <= e.size && p.head_len <= kHeadLen;
    if (same && p.head_len > 0) {
      uint8_t head[kHeadLen];
      if (!fs.read(e.path, 0, head, p.head_len) || !fs.unchanged()) {
        r->interrupted = true;
        return false;
      }
      same = crc32(0, head, p.head_len) == p.head_crc;
    }
    if (!same) {
      p = Progress{};
      restarted = true;
      seen.stable = 0;
    }
  }

  uint64_t limit =
      settled_end(e.size, seen.size, seen.at_ms, now_ms, cfg_.settle_ms);
  if (limit == e.size) {
    seen.stable = limit;
  } else {
    limit = seen.stable;
    *pending = true;
  }

  /* Progress reaches NVS every save_every bytes and when the file is done,
   * not per chunk: each save is a flash write. What a crash loses is only
   * re-offered to the sink, which has to absorb duplicates anyway. */
  uint64_t saved = p.offset;
  bool dirty = false;
  auto flush = [&]() {
    if (!dirty) return true;
    if (!store_->save(e.path, p)) {
      r->store_failed = true;
      return false;
    }
    saved = p.offset;
    dirty = false;
    return true;
  };

  for (;;) {
    const uint64_t end =
        chunk_end(p.offset, limit, record_size, cfg_.max_chunk);
    if (end <= p.offset) break;
    const size_t len = static_cast<size_t>(end - p.offset);
    buf_.resize(len);
    if (!fs.read(e.path, p.offset, buf_.data(), len) || !fs.unchanged()) {
      r->interrupted = true;
      flush();
      return false;
    }
    if (!sink(Chunk{e.path, p.offset, buf_.data(), len, restarted})) {
      r->sink_refused = true;
      flush();
      return false;
    }

    Progress next = p;
    next.offset = end;
    if (next.head_len < kHeadLen) {
      const uint32_t hl =
          static_cast<uint32_t>(std::min<uint64_t>(kHeadLen, end));
      uint8_t head[kHeadLen];
      if (p.offset == 0) {
        std::copy(buf_.begin(), buf_.begin() + hl, head);
      } else if (!fs.read(e.path, 0, head, hl) || !fs.unchanged()) {
        /* Delivered but not recorded: the sink sees these bytes again,
         * which an idempotent server absorbs. Losing them would not be. */
        r->interrupted = true;
        flush();
        return false;
      }
      next.head_len = hl;
      next.head_crc = crc32(0, head, hl);
    }
    p = next;
    dirty = true;
    restarted = false;
    ++r->chunks;
    r->bytes += len;
    if (p.offset - saved >= cfg_.save_every && !flush()) return false;
  }
  return flush();
}

}  // namespace espos_usb_storage
