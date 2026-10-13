#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""Lay one build's flash images out for another partition table.

Used by espos_merge_firmware.py --table: a release builds each chip ONCE and
still ships a full-flash image per flash size. That works because nothing in
the app depends on the table or the flash size. The partition table is its
own 3 KB image, the flash size the running firmware uses is read from the
BOOTLOADER's header (esptool rewrites it when merging), and the app finds its
partitions by name at runtime. So a variant image is the same bootloader and
app, a different table, the app at that table's slot, and the data partitions
moved -- LittleFS ones resized to their new partition.

The OTA image is identical for every variant, which is the constraint this
enforces: the app must fit EVERY app slot of every table a device may have
been flashed with, so a variant whose slots are too small is an error, not a
smaller image.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

BLOCK_SIZE = 4096
# LittleFS's own LFS_NAME_MAX: mounting with less than the image's name_max
# fails, and the image keeps its own value either way.
LFS_NAME_MAX = 255
FLASH_SIZES = ("1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB")


class VariantError(RuntimeError):
    pass


@dataclass(frozen=True)
class Partition:
    name: str
    type: str
    subtype: str
    offset: int
    size: int


@dataclass(frozen=True)
class Placement:
    """One built image and where it goes in the variant."""
    old_offset: int
    new_offset: int
    # None: copy the image as built. Otherwise the LittleFS partition size to
    # resize the image to.
    resize_to: int | None = None


def _int(text: str) -> int:
    text = text.strip()
    m = re.fullmatch(r"(\d+)([KM])", text, re.IGNORECASE)
    if m:
        return int(m.group(1)) * (1024 if m.group(2).upper() == "K" else 1024 * 1024)
    return int(text, 0)


def parse_csv(text: str) -> list[Partition]:
    """Parse a table CSV whose offsets are all filled in -- what
    gen_esp32part.py prints when it converts a binary table back."""
    parts = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5 or not fields[3] or not fields[4]:
            raise VariantError(f"partition line without an offset or size: {line!r}")
        parts.append(Partition(fields[0], fields[1], fields[2], _int(fields[3]), _int(fields[4])))
    return parts


def normalise_flash_size(text: str) -> str:
    size = text.strip().upper()
    if not size.endswith("MB"):
        size += "MB"
    if size not in FLASH_SIZES:
        raise VariantError(f"flash size '{text}': one of {', '.join(FLASH_SIZES)}")
    return size


def _gen(gen_esp32part: Path, *args: str) -> None:
    result = subprocess.run([sys.executable, str(gen_esp32part), "--quiet", *args],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise VariantError((result.stderr or result.stdout).strip())


def read_binary_table(gen_esp32part: Path, binary: Path) -> list[Partition]:
    with tempfile.TemporaryDirectory() as tmp:
        csv = Path(tmp) / "table.csv"
        _gen(gen_esp32part, str(binary), str(csv))
        return parse_csv(csv.read_text())


def build_table(gen_esp32part: Path, csv: Path, out: Path, table_offset: int,
                flash_size: str) -> list[Partition]:
    """CSV -> binary table, checked against the flash size. Returns the
    partitions with every offset resolved."""
    _gen(gen_esp32part, "--offset", hex(table_offset), "--flash-size", flash_size,
         str(csv), str(out))
    return read_binary_table(gen_esp32part, out)


def plan(built: list[Partition], variant: list[Partition], table_offset: int,
         images: dict[int, int], max_percent: float = 90.0
         ) -> tuple[list[Placement], list[str]]:
    """Where each built image goes in the variant table.

    images maps each flashed offset to its file size. Offsets below the
    partition table (the bootloader) stay put; the table itself is the
    caller's to replace. Returns the placements and warnings."""
    by_offset = {p.offset: p for p in built}
    by_name = {p.name: p for p in variant}
    app_slots = [p for p in variant if p.type == "app"]
    # Settings, keys and the Signal K token live in nvs, which the firmware
    # finds by name. A variant may move it, never shrink or drop it: a
    # firmware that fits its settings on one board must fit them on all.
    for old in built:
        if old.type == "data" and old.subtype in ("nvs", "nvs_keys"):
            new = by_name.get(old.name)
            if new is None or new.subtype != old.subtype:
                raise VariantError(f"the table has no {old.subtype} partition "
                                   f"'{old.name}', where the firmware keeps its settings")
            if new.size < old.size:
                raise VariantError(f"'{old.name}' is {new.size} bytes in the table and "
                                   f"{old.size} in the build; settings partitions never shrink")
    placements, warnings = [], []
    for offset, length in sorted(images.items()):
        if offset < table_offset:
            placements.append(Placement(offset, offset))
            continue
        if offset == table_offset:
            continue
        old = by_offset.get(offset)
        if old is None:
            raise VariantError(f"the build flashes an image at {offset:#x}, where its "
                               "own table has no partition")
        new = by_name.get(old.name)
        if new is None:
            raise VariantError(f"the table has no '{old.name}' partition, which the "
                               "build flashes")
        if new.type != old.type or new.subtype != old.subtype:
            raise VariantError(f"'{old.name}' is {old.type}/{old.subtype} in the build "
                               f"and {new.type}/{new.subtype} in the table")
        if old.type == "app":
            # The OTA image is the same file on every variant, so it has to fit
            # the smallest slot an update can land in, not only this one.
            for slot in app_slots:
                if length > slot.size:
                    raise VariantError(
                        f"the app is {length} bytes and '{slot.name}' holds {slot.size}: "
                        "a board flashed with this table could never take an update")
                if length * 100 > slot.size * max_percent:
                    warnings.append(f"the app fills {length * 100 / slot.size:.1f}% of "
                                    f"'{slot.name}' ({slot.size} bytes)")
            placements.append(Placement(offset, new.offset))
            continue
        if new.size == old.size:
            if length > new.size:
                raise VariantError(f"'{old.name}' image is larger than its partition")
            placements.append(Placement(offset, new.offset))
        elif old.subtype in ("littlefs", "0x83"):
            placements.append(Placement(offset, new.offset, resize_to=new.size))
        else:
            raise VariantError(
                f"'{old.name}' is {old.size} bytes in the build and {new.size} in the "
                f"table; only LittleFS images can be resized, so keep {old.subtype} "
                "partitions the same size in every table")
    return placements, warnings


def resize_littlefs(image: bytes, new_size: int) -> bytes:
    """The same files in a LittleFS image of new_size bytes."""
    # Imported here: only a variant with a resized partition needs it, and the
    # plain merge stays standard-library only.
    from littlefs import LittleFS, UserContext

    if len(image) % BLOCK_SIZE or new_size % BLOCK_SIZE:
        raise VariantError("LittleFS images and partitions are whole 4 KB blocks")
    old_count, new_count = len(image) // BLOCK_SIZE, new_size // BLOCK_SIZE
    try:
        return _resize(LittleFS, UserContext, image, old_count, new_count, new_size)
    except VariantError:
        raise
    except Exception as e:  # littlefs raises its own LittleFSError
        raise VariantError(f"LittleFS image could not be resized to {new_size} bytes: {e}") from e


def _resize(LittleFS, UserContext, image: bytes, old_count: int, new_count: int,
            new_size: int) -> bytes:
    ctx = UserContext(len(image))
    ctx.buffer[:] = image
    old = LittleFS(context=ctx, block_size=BLOCK_SIZE, block_count=old_count,
                   name_max=LFS_NAME_MAX, mount=False)
    try:
        old.mount()
    except Exception as e:
        raise VariantError(f"not a LittleFS image with 4 KB blocks: {e}") from e
    if new_count >= old_count:
        # Grown in place: the superblock keeps the build's own settings.
        ctx.buffer.extend(b"\xff" * (new_size - len(image)))
        old.fs_grow(new_count)
        old.unmount()
        return bytes(ctx.buffer)
    # Shrinking cannot happen in place, so the files are copied into a new
    # image with the build's settings. Custom attributes are not copied; the
    # images espOS builds carry none.
    stat = old.fs_stat()
    new = LittleFS(block_size=BLOCK_SIZE, block_count=new_count,
                   name_max=stat.name_max, disk_version=stat.disk_version)
    try:
        for root, dirs, files in old.walk("/"):
            for d in dirs:
                new.makedirs(f"{root.rstrip('/')}/{d}", exist_ok=True)
            for f in files:
                path = f"{root.rstrip('/')}/{f}"
                with old.open(path, "rb") as src:
                    data = src.read()
                with new.open(path, "wb") as dst:
                    dst.write(data)
    except Exception as e:
        raise VariantError(f"the files do not fit {new_size} bytes: {e}") from e
    return bytes(new.context.buffer)
