/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESPOS_USB_STORAGE_FOLLOWER_H_
#define ESPOS_USB_STORAGE_FOLLOWER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

/// Follows append-only files on a card someone else writes, and hands each
/// new run of bytes to a sink exactly once.
///
/// Everything here is driver-free so it is host-tested
/// (test/host/espos_usb_storage_test): the device supplies a read-only view
/// of the card and an NVS-backed progress store, the tests supply fakes.
namespace espos_usb_storage {

struct FileEntry {
  std::string path;  ///< Relative to the volume root, '/'-separated.
  uint64_t size;
};

/// Read-only access to the card. Every call may fail; a failure ends the pass
/// and the follower tries again on the next one.
class FsView {
 public:
  virtual ~FsView() = default;
  /// Regular files directly inside `dir` (no recursion).
  virtual bool list(const std::string& dir, std::vector<FileEntry>* out) = 0;
  /// Exactly `len` bytes at `offset`, or false.
  virtual bool read(const std::string& path, uint64_t offset, uint8_t* buf,
                    size_t len) = 0;
  /// False once the card's owner has written since the pass began. Checked
  /// after every read and before every hand-off: a byte read while the host
  /// was writing may belong to a half-updated file.
  virtual bool unchanged() = 0;
};

/// How far a file has been handed off, and a fingerprint of its head so a
/// file deleted and recreated under the same name starts again from zero.
struct Progress {
  uint64_t offset = 0;
  uint32_t head_len = 0;
  uint32_t head_crc = 0;
};

class ProgressStore {
 public:
  virtual ~ProgressStore() = default;
  virtual bool load(const std::string& path, Progress* out) = 0;
  virtual bool save(const std::string& path, const Progress& p) = 0;
  virtual void erase(const std::string& path) = 0;
  /// Every path with stored progress, for pruning files that are gone.
  virtual std::vector<std::string> paths() = 0;
};

struct Chunk {
  const std::string& path;
  uint64_t offset;  ///< Where `data` starts in the file.
  const uint8_t* data;
  size_t len;      ///< Always a whole number of records.
  bool restarted;  ///< The file was replaced; earlier chunks are stale.
};

/// Returns true once the chunk is safely delivered. False leaves progress
/// where it was and ends the pass, so the same bytes are offered again later
/// (the server being out of range is the normal case for this).
using ChunkSink = std::function<bool(const Chunk&)>;

struct Watch {
  std::string dir;           ///< e.g. "realbathy"
  std::string suffix;        ///< e.g. ".trl", matched case-insensitively
  uint32_t record_size = 1;  ///< Only whole records are handed off.
};

struct FollowerConfig {
  std::vector<Watch> watches;
  /// A size must have been seen this long ago before bytes up to it are
  /// read. A host flushes a file's directory entry and its data in no
  /// guaranteed order, so the newest tail can still be unwritten sectors.
  uint32_t settle_ms = 30000;
  size_t max_chunk = 16 * 1024;
  /// How many handed-off bytes may go unrecorded in NVS before progress is
  /// saved mid-file; the end of a file always saves.
  uint64_t save_every = 256 * 1024;
};

/// What one pass did.
struct PassResult {
  /// Every watched file is handed off as far as it may be.
  bool complete = false;
  /// The host wrote, or a read failed.
  bool interrupted = false;
  bool sink_refused = false;
  /// Progress could not be saved; the pass stopped rather than resend
  /// everything after the next reboot.
  bool store_failed = false;
  size_t chunks = 0;
  uint64_t bytes = 0;
};

/// The settled end of a file: how far it may be read now. Pure, exposed for
/// the tests.
uint64_t settled_end(uint64_t size_now, uint64_t seen_size, int64_t seen_at_ms,
                     int64_t now_ms, uint32_t settle_ms);

/// The end of the next chunk from `offset`: whole records only, at most
/// `max_chunk` bytes (rounded down to a record, but never below one record).
uint64_t chunk_end(uint64_t offset, uint64_t limit, uint32_t record_size,
                   size_t max_chunk);

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);

bool suffix_matches(const std::string& name, const std::string& suffix);

class Follower {
 public:
  Follower(FollowerConfig cfg, ProgressStore* store);

  /// One pass over every watch. Call it only while the host has been quiet
  /// for a while; the FsView aborts the pass if that changes. `write_gen` is
  /// the card's write generation: a change restarts every file's settle time,
  /// since the host may have filled a tail whose size was already visible.
  PassResult run_pass(FsView& fs, const ChunkSink& sink, int64_t now_ms,
                      uint32_t write_gen = 0);

 private:
  struct Seen {
    uint64_t size;
    int64_t at_ms;
    uint32_t gen;
    /// The largest size that has settled since the file was last replaced:
    /// a file that grew again still hands off up to here.
    uint64_t stable;
  };

  /// Hands off one file; false ends the pass.
  bool follow_file(FsView& fs, const ChunkSink& sink, const FileEntry& e,
                   uint32_t record_size, int64_t now_ms, uint32_t write_gen,
                   PassResult* r, bool* pending);

  FollowerConfig cfg_;
  ProgressStore* store_;
  std::map<std::string, Seen> seen_;
  std::vector<uint8_t> buf_;
};

}  // namespace espos_usb_storage

#endif  // ESPOS_USB_STORAGE_FOLLOWER_H_
