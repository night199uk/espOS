/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The follower decides which bytes of a file someone else is writing are safe
 * to hand on. A mistake is either a lost recording or a half-written record
 * uploaded as depth data, so every rule it applies has a case here.
 */
#include <map>
#include <string>
#include <vector>

#include "espos_usb_storage/follower.h"
#include "unity.h"

using namespace espos_usb_storage;

namespace
{

constexpr uint32_t kSettle = 30000;

struct FakeFs : FsView {
    std::map<std::string, std::vector<uint8_t>> files;
    int writes_after_reads = -1;  // host writes once this many reads are done
    int reads = 0;
    bool written = false;

    bool list(const std::string &dir, std::vector<FileEntry> *out) override
    {
        for (auto &[p, d] : files)
            if (p.compare(0, dir.size() + 1, dir + "/") == 0) out->push_back({ p, d.size() });
        return true;
    }
    bool read(const std::string &path, uint64_t off, uint8_t *buf, size_t len) override
    {
        auto it = files.find(path);
        if (it == files.end() || off + len > it->second.size()) return false;
        std::copy(it->second.begin() + off, it->second.begin() + off + len, buf);
        if (++reads == writes_after_reads) written = true;
        return true;
    }
    bool unchanged() override { return !written; }
};

struct MemStore : ProgressStore {
    std::map<std::string, Progress> m;
    bool load(const std::string &p, Progress *out) override
    {
        auto it = m.find(p);
        if (it == m.end()) return false;
        *out = it->second;
        return true;
    }
    int saves = 0;
    bool fail_saves = false;
    bool save(const std::string &p, const Progress &pr) override
    {
        if (fail_saves) return false;
        ++saves;
        m[p] = pr;
        return true;
    }
    void erase(const std::string &p) override { m.erase(p); }
    std::vector<std::string> paths() override
    {
        std::vector<std::string> out;
        for (auto &[p, _] : m) out.push_back(p);
        return out;
    }
};

struct Got {
    std::string path;
    uint64_t offset;
    std::vector<uint8_t> data;
    bool restarted;
};

std::vector<uint8_t> bytes(size_t n, uint8_t seed)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(seed + i);
    return v;
}

FollowerConfig trl(size_t max_chunk = 16 * 1024)
{
    FollowerConfig c;
    c.watches = { { "realbathy", ".trl", 24 } };
    c.settle_ms = kSettle;
    c.max_chunk = max_chunk;
    return c;
}

ChunkSink collect(std::vector<Got> *got, bool accept = true)
{
    return [got, accept](const Chunk &c) {
        got->push_back({ c.path, c.offset, { c.data, c.data + c.len }, c.restarted });
        return accept;
    };
}

}  // namespace

TEST_CASE("crc32 is the standard reflected CRC-32", "[usb_storage]")
{
    const char *s = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(0, (const uint8_t *)s, 9));
}

TEST_CASE("suffix match ignores case, FAT does too", "[usb_storage]")
{
    TEST_ASSERT_TRUE(suffix_matches("realbathy/1A2B.TRL", ".trl"));
    TEST_ASSERT_TRUE(suffix_matches("realbathy/x.trl", ".trl"));
    TEST_ASSERT_FALSE(suffix_matches("realbathy/x.trk", ".trl"));
    TEST_ASSERT_FALSE(suffix_matches("rl", ".trl"));
}

TEST_CASE("chunk_end hands off whole records only", "[usb_storage]")
{
    TEST_ASSERT_EQUAL_UINT64(48, chunk_end(0, 50, 24, 1024));
    TEST_ASSERT_EQUAL_UINT64(24, chunk_end(24, 47, 24, 1024));
    TEST_ASSERT_EQUAL_UINT64(24 + 984, chunk_end(24, 100000, 24, 1000));  // 41 records
    // a budget smaller than one record still moves one record
    TEST_ASSERT_EQUAL_UINT64(24, chunk_end(0, 100, 24, 10));
    TEST_ASSERT_EQUAL_UINT64(10, chunk_end(10, 5, 24, 1024));
}

TEST_CASE("settled_end waits for a size to stay put", "[usb_storage]")
{
    TEST_ASSERT_EQUAL_UINT64(0, settled_end(100, 100, 0, kSettle - 1, kSettle));
    TEST_ASSERT_EQUAL_UINT64(100, settled_end(100, 100, 0, kSettle, kSettle));
    TEST_ASSERT_EQUAL_UINT64(0, settled_end(120, 100, 0, kSettle * 2, kSettle));
}

TEST_CASE("a new file is handed off only after it settles", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(24 * 3 + 5, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;

    PassResult r = f.run_pass(fs, collect(&got), 0);
    TEST_ASSERT_EQUAL(0, got.size());
    TEST_ASSERT_FALSE(r.complete);

    r = f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL_UINT64(0, got[0].offset);
    TEST_ASSERT_EQUAL(72, got[0].data.size());  // the 5-byte tail is not a record
    TEST_ASSERT_EQUAL_UINT64(72, store.m["realbathy/a.trl"].offset);
    TEST_ASSERT_TRUE(r.complete);
}

TEST_CASE("only the appended records go out next time", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    auto &file = fs.files["realbathy/a.trl"];
    file = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(1, got.size());

    auto more = bytes(48, 99);
    file.insert(file.end(), more.begin(), more.end());
    f.run_pass(fs, collect(&got), kSettle + 1);
    TEST_ASSERT_EQUAL(1, got.size());  // grew: settles again first
    f.run_pass(fs, collect(&got), 2 * kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_EQUAL_UINT64(48, got[1].offset);
    TEST_ASSERT_EQUAL_MEMORY(more.data(), got[1].data.data(), 48);
    TEST_ASSERT_FALSE(got[1].restarted);
}

TEST_CASE("a refused chunk is offered again unchanged", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got, false), 0);
    PassResult r = f.run_pass(fs, collect(&got, false), kSettle);
    TEST_ASSERT_TRUE(r.sink_refused);
    TEST_ASSERT_FALSE(r.complete);
    TEST_ASSERT_EQUAL(0, store.m.count("realbathy/a.trl"));

    f.run_pass(fs, collect(&got), kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_EQUAL_UINT64(0, got[1].offset);
    TEST_ASSERT_TRUE(got[0].data == got[1].data);
}

TEST_CASE("a host write during the pass discards what was read", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(24 * 10, 1);
    Follower f(trl(48), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);

    fs.writes_after_reads = 2;  // the second chunk's read sees the host write
    PassResult r = f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_TRUE(r.interrupted);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL_UINT64(48, store.m["realbathy/a.trl"].offset);
}

TEST_CASE("a replaced file starts again from zero", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(96, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(1, got.size());

    // deleted and recorded anew under the same name, longer than before
    fs.files["realbathy/a.trl"] = bytes(120, 50);
    f.run_pass(fs, collect(&got), kSettle + 1);
    f.run_pass(fs, collect(&got), 2 * kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_EQUAL_UINT64(0, got[1].offset);
    TEST_ASSERT_EQUAL(120, got[1].data.size());
    TEST_ASSERT_TRUE(got[1].restarted);
}

TEST_CASE("a shrunk file starts again from zero", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(96, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);

    fs.files["realbathy/a.trl"] = bytes(48, 1);
    f.run_pass(fs, collect(&got), kSettle + 1);
    f.run_pass(fs, collect(&got), 2 * kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_TRUE(got[1].restarted);
    TEST_ASSERT_EQUAL(48, got[1].data.size());
}

TEST_CASE("a file shorter than the fingerprint keeps its progress", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    auto &file = fs.files["realbathy/a.trl"];
    file = bytes(24, 1);  // one record: the head fingerprint covers 24 bytes
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL_UINT32(24, store.m["realbathy/a.trl"].head_len);

    auto more = bytes(48, 24 + 1);  // continues the same byte sequence
    file.insert(file.end(), more.begin(), more.end());
    f.run_pass(fs, collect(&got), kSettle + 1);
    f.run_pass(fs, collect(&got), 2 * kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_EQUAL_UINT64(24, got[1].offset);
    TEST_ASSERT_FALSE(got[1].restarted);
    TEST_ASSERT_EQUAL_UINT32(32, store.m["realbathy/a.trl"].head_len);
}

TEST_CASE("progress for deleted files is pruned", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(48, 1);
    store.m["realbathy/gone.trl"] = Progress { 24, 24, 0 };
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    TEST_ASSERT_EQUAL(0, store.m.count("realbathy/gone.trl"));
}

TEST_CASE("files outside the watch are left alone", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trk"] = bytes(48, 1);
    fs.files["other/b.trl"] = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(0, got.size());
}

TEST_CASE("chunks split at the budget and arrive in order", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/b.trl"] = bytes(24 * 5, 1);
    fs.files["realbathy/a.trl"] = bytes(24 * 2, 7);
    Follower f(trl(48), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(4, got.size());
    TEST_ASSERT_EQUAL_STRING("realbathy/a.trl", got[0].path.c_str());
    TEST_ASSERT_EQUAL_UINT64(0, got[1].offset);
    TEST_ASSERT_EQUAL_UINT64(48, got[2].offset);
    TEST_ASSERT_EQUAL_UINT64(96, got[3].offset);
    TEST_ASSERT_EQUAL(24, got[3].data.size());
}

TEST_CASE("progress is saved per file, not per chunk", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(24 * 100, 1);
    FollowerConfig cfg = trl(48);
    cfg.save_every = 24 * 40;
    Follower f(cfg, &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(50, got.size());
    TEST_ASSERT_EQUAL(3, store.saves);  // at 40 and 80 records, and the end
    TEST_ASSERT_EQUAL_UINT64(2400, store.m["realbathy/a.trl"].offset);
}

TEST_CASE("a refused chunk still records what went before it", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(24 * 4, 1);
    Follower f(trl(24), &store);
    int n = 0;
    ChunkSink two = [&n](const Chunk &) { return ++n <= 2; };
    f.run_pass(fs, two, 0);
    PassResult r = f.run_pass(fs, two, kSettle);
    TEST_ASSERT_TRUE(r.sink_refused);
    TEST_ASSERT_EQUAL_UINT64(48, store.m["realbathy/a.trl"].offset);
}

TEST_CASE("a failed save stops the pass", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    store.fail_saves = true;
    fs.files["realbathy/a.trl"] = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    PassResult r = f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_TRUE(r.store_failed);
    TEST_ASSERT_FALSE(r.complete);
}

TEST_CASE("a stored fingerprint longer than ours restarts the file", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(240, 1);
    store.m["realbathy/a.trl"] = Progress { 48, 200, 0 };
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0);
    f.run_pass(fs, collect(&got), kSettle);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL_UINT64(0, got[0].offset);
    TEST_ASSERT_TRUE(got[0].restarted);
}

TEST_CASE("a host write anywhere restarts the settle time", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    fs.files["realbathy/a.trl"] = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got), 0, 1);
    f.run_pass(fs, collect(&got), kSettle, 2);  // same size, but the host wrote
    TEST_ASSERT_EQUAL(0, got.size());
    f.run_pass(fs, collect(&got), 2 * kSettle, 2);
    TEST_ASSERT_EQUAL(1, got.size());
}

TEST_CASE("growth does not hold back what had already settled", "[usb_storage]")
{
    FakeFs fs;
    MemStore store;
    auto &file = fs.files["realbathy/a.trl"];
    file = bytes(48, 1);
    Follower f(trl(), &store);
    std::vector<Got> got;
    f.run_pass(fs, collect(&got, false), 0);
    f.run_pass(fs, collect(&got, false), kSettle);  // settled, sink away
    TEST_ASSERT_EQUAL(1, got.size());

    auto more = bytes(48, 49);
    file.insert(file.end(), more.begin(), more.end());
    PassResult r = f.run_pass(fs, collect(&got), kSettle + 1);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_EQUAL(48, got[1].data.size());  // the settled part only
    TEST_ASSERT_FALSE(r.complete);
    f.run_pass(fs, collect(&got), 2 * kSettle + 1);
    TEST_ASSERT_EQUAL(3, got.size());
    TEST_ASSERT_EQUAL_UINT64(48, got[2].offset);
}
