#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""Merge an ESP-IDF build's flash images into one binary.

Reads the offset/file map the build already worked out
(`flasher_args.json`), so the offsets here can never drift from what the
partition table and bootloader actually need -- which is the whole reason
this is not a hand-written list of addresses.

    tools/espos_merge_firmware.py --build-dir build --chip esp32c6 \
        --out firmware-merged.bin

A merged image is what a USB flash wants: one file at offset 0, no
partition table to line up by hand. The OTA image is the app binary on its
own, which the build already produced.

    --require <name>   fail if that partition is missing from the build

`--table <csv> --flash-size <n>MB` lays the same build out for another
partition table instead: one build, one full-flash image per flash size
(espos_flash_variant.py says why that is safe, and what it refuses).

Use `--require` for a data partition whose absence still yields a BOOTABLE
image, because those are the ones that go unnoticed: an espOS firmware
merged without its `storage` partition boots and serves a placeholder page
that reads as a firmware bug, and one merged without a wake-word model
boots with a dead wake word that no OTA can repair.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                         help="e.g. build")
    parser.add_argument("--chip", required=True, help="e.g. esp32p4")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--require", action="append", default=[], metavar="NAME",
        help="fail if this partition is missing from the build (repeatable). "
             "Matched against the flasher_args path, so 'storage', "
             "'srmodels' or a filename all work. Use it for a partition "
             "whose absence still leaves a bootable image -- those are the "
             "ones nobody notices.")
    parser.add_argument(
        "--table", type=Path, metavar="CSV",
        help="lay the build out for this partition table instead of its own")
    parser.add_argument(
        "--flash-size", metavar="SIZE",
        help="flash size written to the bootloader header, e.g. 8MB; "
             "required with --table")
    parser.add_argument(
        "--gen-esp32part", type=Path, metavar="PATH",
        help="ESP-IDF's gen_esp32part.py, for --table (default: "
             "$IDF_PATH/components/partition_table/gen_esp32part.py)")
    parser.add_argument(
        "--max-percent", type=float, default=90.0,
        help="with --table, warn when the app fills more of a slot than this")
    args = parser.parse_args()
    if args.table and not args.flash_size:
        parser.error("--table needs --flash-size")

    flasher_args_path = args.build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        print(f"error: {flasher_args_path} not found — build the firmware first",
              file=sys.stderr)
        return 1

    flasher_args = json.loads(flasher_args_path.read_text())
    # The app's own path, so an unresolvable entry can be told apart from a
    # data partition that simply was not built.
    app_rel = flasher_args.get("app", {}).get("file", "")
    flash_settings = flasher_args["flash_settings"]
    flash_files = flasher_args["flash_files"]

    # Every file, because a build lays its outputs out in several places and
    # resolve() matches by basename. NOT printed: a build dir holds thousands
    # of objects and a pip venv, and dumping them buried the one line that
    # mattered -- the error saying which partition was missing.
    all_files = [p for p in args.build_dir.rglob("*") if p.is_file()]

    # flasher_args.json's paths mirror idf.py's build/bootloader/,
    # build/partition_table/ layout; resolve by role as well so a renamed
    # output still lands.
    #
    # Skipping otadata is harmless: all-0xFF means "boot the first OTA slot",
    # which is what a freshly flashed device should do. Skipping a DATA
    # partition often is not, and the trouble is that it still boots -- hence
    # --require, which turns "not produced by this build" into an error for
    # the partitions a project cannot ship without.
    by_basename = {p.name: p for p in all_files}
    mandatory_roles = {
        "bootloader": "bootloader.bin",
        "partition": "partitions.bin",
    }
    optional_roles = {
        "ota_data": "ota_data_initial.bin",
        "srmodels": "srmodels.bin",
        "model": "srmodels.bin",
    }

    def resolve(rel_path: str) -> Path | None:
        direct = args.build_dir / rel_path
        if direct.is_file():
            return direct
        found = by_basename.get(Path(rel_path).name)
        if found is not None:
            return found
        lower = rel_path.lower()
        for keyword, override_name in mandatory_roles.items():
            if keyword in lower:
                if override_name in by_basename:
                    return by_basename[override_name]
                print(f"error: could not locate '{rel_path}' (role: {keyword}) "
                      f"anywhere under {args.build_dir}", file=sys.stderr)
                sys.exit(1)
        for keyword, override_name in optional_roles.items():
            if keyword in lower:
                return by_basename.get(override_name)
        # What is left is either the app image or a data partition this
        # build did not produce. Distinguish them by asking flasher_args
        # which offset the app lives at, rather than guessing from the name:
        # a project is free to call its app anything, and treating an absent
        # data partition as a missing app produced the confusing
        # "could not locate 'storage.bin' (app image)".
        if rel_path == app_rel:
            if "firmware.bin" in by_basename:
                return by_basename["firmware.bin"]
            print(f"error: could not locate the app image '{rel_path}' "
                  f"anywhere under {args.build_dir}", file=sys.stderr)
            sys.exit(1)
        return None  # a data partition this build did not produce

    images: dict[int, Path] = {}
    for offset, rel_path in sorted(flash_files.items(), key=lambda kv: int(kv[0], 16)):
        resolved = resolve(rel_path)
        if resolved is None:
            lower = rel_path.lower()
            wanted = [r for r in args.require if r.lower() in lower]
            if wanted:
                print(f"error: --require {wanted[0]} was given, but '{rel_path}' "
                      f"was not produced by this build. Merging without it "
                      f"would still yield a bootable image -- which is exactly "
                      f"why this is an error rather than a warning.",
                      file=sys.stderr)
                return 1
            print(f"skipping {offset} ({rel_path}): not produced by this build")
            continue
        images[int(offset, 16)] = resolved

    flash_size = flash_settings["flash_size"]
    with tempfile.TemporaryDirectory() as tmp:
        if args.table:
            # Imported here so a plain merge needs nothing beside this file.
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import espos_flash_variant as variant
            try:
                flash_size = variant.normalise_flash_size(args.flash_size)
                images = lay_out_variant(variant, args, flasher_args, images,
                                         flash_size, Path(tmp))
            except variant.VariantError as e:
                print(f"error: {args.table}: {e}", file=sys.stderr)
                return 1
        cmd = [
            "esptool.py", "--chip", args.chip, "merge_bin",
            "-o", str(args.out),
            "--flash_mode", flash_settings["flash_mode"],
            "--flash_freq", flash_settings["flash_freq"],
            "--flash_size", flash_size,
        ]
        for offset, path in sorted(images.items()):
            cmd += [hex(offset), str(path)]
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
    return 0


def lay_out_variant(variant, args, flasher_args: dict, images: dict[int, Path],
                    flash_size: str, tmp: Path) -> dict[int, Path]:
    gen = args.gen_esp32part
    if gen is None:
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise variant.VariantError("pass --gen-esp32part or set IDF_PATH")
        gen = Path(idf) / "components" / "partition_table" / "gen_esp32part.py"
    if not gen.is_file():
        raise variant.VariantError(f"{gen} not found")
    table_offset = int(flasher_args["partition-table"]["offset"], 16)
    if table_offset not in images:
        raise variant.VariantError("the build flashed no partition table")
    built = variant.read_binary_table(gen, images[table_offset])
    table_bin = tmp / "partition-table.bin"
    wanted = variant.build_table(gen, args.table, table_bin, table_offset, flash_size)
    placements, warnings = variant.plan(
        built, wanted, table_offset,
        {offset: path.stat().st_size for offset, path in images.items()},
        args.max_percent)
    for w in warnings:
        print(f"warning: {args.table}: {w}", file=sys.stderr)
    out = {table_offset: table_bin}
    for p in placements:
        src = images[p.old_offset]
        if p.resize_to is not None:
            resized = tmp / f"{p.new_offset:#x}-{src.name}"
            resized.write_bytes(variant.resize_littlefs(src.read_bytes(), p.resize_to))
            src = resized
        out[p.new_offset] = src
    return out


if __name__ == "__main__":
    raise SystemExit(main())
