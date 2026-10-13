# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""One build laid out for another flash size: the app moves to the new slot,
LittleFS keeps its files, and a table whose slots the app outgrows is
refused rather than shipped."""

import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import espos_flash_variant as variant  # noqa: E402
from espos_flash_variant import Partition, Placement, VariantError  # noqa: E402

TOOLS = pathlib.Path(__file__).resolve().parent
TABLES = TOOLS.parent / "components" / "espos_core" / "partitions"
HAVE_LITTLEFS = importlib.util.find_spec("littlefs") is not None
# The merge runs the esptool.py script, so the module alone is not enough.
HAVE_ESPTOOL = shutil.which("esptool.py") is not None
# ESP-IDF's own table generator; CI fetches it at the pinned IDF version.
GEN = os.environ.get("ESPOS_GEN_ESP32PART") or (
    os.path.join(os.environ["IDF_PATH"], "components", "partition_table", "gen_esp32part.py")
    if os.environ.get("IDF_PATH") else "")

K = 1024
M = 1024 * K


def table(*rows):
    return [Partition(*r) for r in rows]


BUILT = table(
    ("nvs", "data", "nvs", 0x9000, 48 * K),
    ("otadata", "data", "ota", 0x15000, 8 * K),
    ("ota_0", "app", "ota_0", 0x60000, 2560 * K),
    ("ota_1", "app", "ota_1", 0x2E0000, 2560 * K),
    ("storage", "data", "littlefs", 0x560000, 576 * K),
)
SMALL = table(
    ("nvs", "data", "nvs", 0x9000, 48 * K),
    ("otadata", "data", "ota", 0x15000, 8 * K),
    ("ota_0", "app", "ota_0", 0x30000, 3 * M),
    ("ota_1", "app", "ota_1", 0x330000, 3 * M),
    ("storage", "data", "littlefs", 0x630000, 1792 * K),
)


class PlanTest(unittest.TestCase):
    def test_moves_each_image_to_its_partition_by_name(self):
        images = {0x2000: 30 * K, 0x8000: 3 * K, 0x15000: 8 * K,
                  0x60000: 1700 * K, 0x560000: 576 * K}
        placements, warnings = variant.plan(BUILT, SMALL, 0x8000, images)
        self.assertEqual(placements, [
            Placement(0x2000, 0x2000),
            Placement(0x15000, 0x15000),
            Placement(0x60000, 0x30000),
            Placement(0x560000, 0x630000, resize_to=1792 * K),
        ])
        self.assertEqual(warnings, [])

    def test_app_that_outgrows_any_slot_is_refused(self):
        # A board on this table would install it over USB and then refuse
        # every update, because the OTA image is the same file.
        tight = [p if p.name != "ota_1" else Partition("ota_1", "app", "ota_1", 0x330000, 1 * M)
                 for p in SMALL]
        with self.assertRaisesRegex(VariantError, "ota_1"):
            variant.plan(BUILT, tight, 0x8000, {0x60000: 1700 * K})

    def test_nearly_full_slot_warns(self):
        _, warnings = variant.plan(BUILT, SMALL, 0x8000, {0x60000: int(2.9 * M)})
        self.assertEqual(len(warnings), 2)

    def test_partition_missing_from_the_table(self):
        no_storage = [p for p in SMALL if p.name != "storage"]
        with self.assertRaisesRegex(VariantError, "storage"):
            variant.plan(BUILT, no_storage, 0x8000, {0x560000: 576 * K})

    def test_non_littlefs_data_must_keep_its_size(self):
        built = BUILT + table(("model", "data", "fat", 0x600000, 1 * M))
        small = SMALL + table(("model", "data", "fat", 0x7B0000, 512 * K))
        with self.assertRaisesRegex(VariantError, "fat"):
            variant.plan(built, small, 0x8000, {0x600000: 1 * M})

    def test_settings_partition_never_shrinks(self):
        smaller = [p if p.name != "nvs" else Partition("nvs", "data", "nvs", 0x9000, 24 * K)
                   for p in SMALL]
        with self.assertRaisesRegex(VariantError, "never shrink"):
            variant.plan(BUILT, smaller, 0x8000, {})
        with self.assertRaisesRegex(VariantError, "settings"):
            variant.plan(BUILT, [p for p in SMALL if p.name != "nvs"], 0x8000, {})

    def test_type_change_is_refused(self):
        changed = [p if p.name != "storage" else Partition("storage", "data", "spiffs", p.offset, p.size)
                   for p in SMALL]
        with self.assertRaisesRegex(VariantError, "spiffs"):
            variant.plan(BUILT, changed, 0x8000, {0x560000: 576 * K})

    def test_parse_csv_reads_sizes_with_units(self):
        parts = variant.parse_csv("# comment\nnvs,data,nvs,0x9000,48K,\nota_0,app,ota_0,0x30000,3M,\n")
        self.assertEqual(parts[1], Partition("ota_0", "app", "ota_0", 0x30000, 3 * M))

    def test_flash_size_spelling(self):
        self.assertEqual(variant.normalise_flash_size("8mb"), "8MB")
        self.assertEqual(variant.normalise_flash_size("16"), "16MB")
        with self.assertRaises(VariantError):
            variant.normalise_flash_size("12MB")


@unittest.skipUnless(HAVE_LITTLEFS, "littlefs-python not installed")
class LittleFSTest(unittest.TestCase):
    def make(self, blocks):
        from littlefs import LittleFS
        fs = LittleFS(block_size=4096, block_count=blocks, name_max=64)
        fs.makedirs("/www", exist_ok=True)
        with fs.open("/www/index.html.gz", "wb") as f:
            f.write(b"x" * 30000)
        return bytes(fs.context.buffer)

    def files(self, image):
        from littlefs import LittleFS, UserContext
        ctx = UserContext(len(image))
        ctx.buffer[:] = image
        fs = LittleFS(context=ctx, block_size=4096, block_count=len(image) // 4096, mount=True)
        with fs.open("/www/index.html.gz", "rb") as f:
            return fs.fs_stat(), f.read()

    def test_grow_keeps_the_files(self):
        out = variant.resize_littlefs(self.make(144), 1792 * K)
        self.assertEqual(len(out), 1792 * K)
        stat, data = self.files(out)
        self.assertEqual((stat.block_count, stat.name_max, data), (448, 64, b"x" * 30000))

    def test_shrink_keeps_the_files(self):
        out = variant.resize_littlefs(self.make(448), 384 * K)
        stat, data = self.files(out)
        self.assertEqual((stat.block_count, stat.name_max, data), (96, 64, b"x" * 30000))

    def test_shrink_below_the_files_is_refused(self):
        with self.assertRaises(VariantError):
            variant.resize_littlefs(self.make(448), 16 * K)

    def test_not_littlefs(self):
        with self.assertRaises(VariantError):
            variant.resize_littlefs(b"\xff" * 64 * K, 128 * K)


@unittest.skipUnless(GEN and os.path.isfile(GEN) and HAVE_LITTLEFS and HAVE_ESPTOOL,
                     "needs gen_esp32part.py (IDF_PATH or ESPOS_GEN_ESP32PART), "
                     "littlefs-python and esptool")
class MergeTest(unittest.TestCase):
    """espos_merge_firmware.py --table end to end, on a fake ESP32-C6 build
    made with espOS's own 16 MB table."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        build = self.tmp / "build"
        (build / "bootloader").mkdir(parents=True)
        (build / "partition_table").mkdir()
        # An image header (magic 0xE9) whose flash size esptool rewrites.
        boot = bytearray(b"\xe9\x03\x02\x40" + b"\x00" * 4092)
        (build / "bootloader" / "bootloader.bin").write_bytes(bytes(boot))
        subprocess.run([sys.executable, GEN, "--quiet", "--flash-size", "16MB",
                        str(TABLES / "16mb.csv"),
                        str(build / "partition_table" / "partition-table.bin")], check=True)
        built = variant.read_binary_table(pathlib.Path(GEN),
                                          build / "partition_table" / "partition-table.bin")
        at = {p.name: p.offset for p in built}
        self.app = b"A" * (1200 * K)
        (build / "app.bin").write_bytes(self.app)
        (build / "ota_data_initial.bin").write_bytes(b"\xff" * 8 * K)
        from littlefs import LittleFS
        size = next(p.size for p in built if p.name == "storage")
        fs = LittleFS(block_size=4096, block_count=size // 4096, name_max=64)
        with fs.open("/index.html.gz", "wb") as f:
            f.write(b"ui")
        (build / "storage.bin").write_bytes(bytes(fs.context.buffer))
        files = {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            hex(at["otadata"]): "ota_data_initial.bin",
            hex(at["ota_0"]): "app.bin",
            hex(at["storage"]): "storage.bin",
        }
        (build / "flasher_args.json").write_text(json.dumps({
            "flash_settings": {"flash_mode": "dio", "flash_freq": "80m", "flash_size": "16MB"},
            "flash_files": files,
            "partition-table": {"offset": "0x8000", "file": files["0x8000"]},
            "app": {"offset": hex(at["ota_0"]), "file": "app.bin"},
        }))
        self.build = build

    def merge(self, csv, size):
        out = self.tmp / f"merged-{size}.bin"
        r = subprocess.run([sys.executable, str(TOOLS / "espos_merge_firmware.py"),
                            "--build-dir", str(self.build), "--chip", "esp32c6",
                            "--out", str(out), "--table", str(csv), "--flash-size", size,
                            "--gen-esp32part", GEN, "--require", "storage"],
                           capture_output=True, text=True)
        return r, out

    def test_4mb_layout(self):
        r, out = self.merge(TABLES / "4mb.csv", "4mb")
        self.assertEqual(r.returncode, 0, r.stderr)
        image = out.read_bytes()
        self.assertLessEqual(len(image), 4 * M)
        # Flash size in the bootloader header: 4 MB is 0x2 in the high nibble.
        self.assertEqual(image[3] >> 4, 0x2)
        with tempfile.NamedTemporaryFile(suffix=".bin") as t:
            t.write(image[0x8000:0x9000])
            t.flush()
            parts = {p.name: p for p in variant.read_binary_table(pathlib.Path(GEN), pathlib.Path(t.name))}
        app_at = parts["ota_0"].offset
        self.assertEqual(image[app_at:app_at + len(self.app)], self.app)
        storage = image[parts["storage"].offset:parts["storage"].offset + parts["storage"].size]
        self.assertEqual(len(storage), parts["storage"].size)
        from littlefs import LittleFS, UserContext
        ctx = UserContext(len(storage))
        ctx.buffer[:] = storage
        fs = LittleFS(context=ctx, block_size=4096, block_count=len(storage) // 4096, mount=True)
        with fs.open("/index.html.gz", "rb") as f:
            self.assertEqual(f.read(), b"ui")

    def test_table_larger_than_the_flash_is_refused(self):
        r, _ = self.merge(TABLES / "16mb.csv", "8MB")
        self.assertEqual(r.returncode, 1)

    def test_app_too_big_for_the_slots_is_refused(self):
        (self.build / "app.bin").write_bytes(b"A" * (2 * M))
        r, _ = self.merge(TABLES / "4mb.csv", "4MB")
        self.assertEqual(r.returncode, 1)
        self.assertIn("could never take an update", r.stderr)


if __name__ == "__main__":
    unittest.main()
