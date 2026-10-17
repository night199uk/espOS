/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The progress store decides what a device resends after a reboot. Progress
 * that belongs to another file skips that file's unsent records without a
 * trace, so the key collision and the record bounds each have a case here,
 * against the real NVS of the linux target.
 */
#include <algorithm>
#include <string>
#include <vector>

#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_progress.h"
#include "unity.h"

using namespace espos_usb_storage;

namespace
{

constexpr const char *kNs = "usbst_test";

/* Found by search: both paths have the CRC32 0xd07df5e3, so both map to the
 * same NVS key. */
const std::string kCollideA = "realbathy/0008ddf9.trl";
const std::string kCollideB = "realbathy/02008894.trl";

void fresh_nvs()
{
    nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(nvs_flash_erase_partition("nvs"));
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
}

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("progress survives a save/load round trip", "[usb_storage][nvs]")
{
    fresh_nvs();
    NvsProgressStore store(kNs);
    const Progress p { 1234567890123ull, 32, 0xdeadbeefu };
    TEST_ASSERT_TRUE(store.save("realbathy/1.trl", p));

    Progress got {};
    TEST_ASSERT_TRUE(store.load("realbathy/1.trl", &got));
    TEST_ASSERT_EQUAL_UINT64(p.offset, got.offset);
    TEST_ASSERT_EQUAL_UINT32(p.head_len, got.head_len);
    TEST_ASSERT_EQUAL_UINT32(p.head_crc, got.head_crc);
    TEST_ASSERT_FALSE(store.load("realbathy/2.trl", &got));
}

TEST_CASE("a colliding path never reads the other path's progress",
          "[usb_storage][nvs]")
{
    TEST_ASSERT_EQUAL_UINT32(
        crc32(0, reinterpret_cast<const uint8_t *>(kCollideA.data()), kCollideA.size()),
        crc32(0, reinterpret_cast<const uint8_t *>(kCollideB.data()), kCollideB.size()));

    fresh_nvs();
    NvsProgressStore store(kNs);
    TEST_ASSERT_TRUE(store.save(kCollideA, Progress { 480, 32, 1 }));

    Progress got {};
    TEST_ASSERT_FALSE(store.load(kCollideB, &got));
    TEST_ASSERT_TRUE(store.load(kCollideA, &got));
    TEST_ASSERT_EQUAL_UINT64(480, got.offset);

    /* The second path evicts the first: both get resent, neither is skipped. */
    TEST_ASSERT_TRUE(store.save(kCollideB, Progress { 24, 24, 2 }));
    TEST_ASSERT_FALSE(store.load(kCollideA, &got));
    TEST_ASSERT_TRUE(store.load(kCollideB, &got));
    TEST_ASSERT_EQUAL_UINT64(24, got.offset);
}

TEST_CASE("paths lists what is stored and forgets what is erased",
          "[usb_storage][nvs]")
{
    fresh_nvs();
    NvsProgressStore store(kNs);
    TEST_ASSERT_TRUE(store.save("realbathy/a.trl", Progress { 24, 24, 1 }));
    TEST_ASSERT_TRUE(store.save("realbathy/b.trl", Progress { 48, 32, 2 }));

    std::vector<std::string> all = store.paths();
    TEST_ASSERT_EQUAL(2, all.size());
    TEST_ASSERT_TRUE(contains(all, "realbathy/a.trl"));
    TEST_ASSERT_TRUE(contains(all, "realbathy/b.trl"));

    store.erase("realbathy/a.trl");
    all = store.paths();
    TEST_ASSERT_EQUAL(1, all.size());
    TEST_ASSERT_TRUE(contains(all, "realbathy/b.trl"));
    Progress got {};
    TEST_ASSERT_FALSE(store.load("realbathy/a.trl", &got));
}

TEST_CASE("paths ignores other namespaces", "[usb_storage][nvs]")
{
    fresh_nvs();
    NvsProgressStore store(kNs);
    NvsProgressStore other("usbst_other");
    TEST_ASSERT_TRUE(store.save("realbathy/a.trl", Progress { 24, 24, 1 }));
    TEST_ASSERT_TRUE(other.save("realbathy/z.trl", Progress { 24, 24, 1 }));

    const std::vector<std::string> all = store.paths();
    TEST_ASSERT_EQUAL(1, all.size());
    TEST_ASSERT_TRUE(contains(all, "realbathy/a.trl"));
}

TEST_CASE("a path of the maximum length is stored, a longer one refused",
          "[usb_storage][nvs]")
{
    fresh_nvs();
    NvsProgressStore store(kNs);
    const std::string longest = "realbathy/" + std::string(512 - 14, 'x') + ".trl";
    TEST_ASSERT_EQUAL(512, longest.size());
    TEST_ASSERT_TRUE(store.save(longest, Progress { 72, 32, 3 }));
    Progress got {};
    TEST_ASSERT_TRUE(store.load(longest, &got));
    TEST_ASSERT_EQUAL_UINT64(72, got.offset);

    const std::string too_long = longest + "x";
    TEST_ASSERT_FALSE(store.save(too_long, Progress { 72, 32, 3 }));
    TEST_ASSERT_FALSE(store.load(too_long, &got));
}

TEST_CASE("a record shorter than its header is not progress", "[usb_storage][nvs]")
{
    fresh_nvs();
    nvs_handle_t h;
    TEST_ESP_OK(nvs_open(kNs, NVS_READWRITE, &h));
    const uint8_t stub[4] = { 1, 2, 3, 4 };
    /* The key the store derives for "" (CRC32 of nothing is 0). */
    TEST_ESP_OK(nvs_set_blob(h, "p00000000", stub, sizeof(stub)));
    TEST_ESP_OK(nvs_commit(h));
    nvs_close(h);

    NvsProgressStore store(kNs);
    Progress got {};
    TEST_ASSERT_FALSE(store.load("", &got));
    TEST_ASSERT_TRUE(store.paths().empty());
}
