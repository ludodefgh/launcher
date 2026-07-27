#!/usr/bin/env python3
"""
Builds the launcher (via `idf.py build`) and combines it with an
already-built guest app binary into a single flashable image, and/or
flashes both straight to a device -- writing the guest binary at the
resolved flash offset for a chosen app slot (e.g. app_slot1).

Two independent build systems, one flash step: the guest app (PlatformIO,
Arduino, a second idf.py project, whatever) is built entirely on its own
and handed to this script as a plain .bin. This script never touches the
guest project -- see README.md "Reusing this launcher" / the design
discussion in issue #11 for why that split was chosen over unifying build
systems.

Requires an ESP-IDF environment (`. $IDF_PATH/export.sh`, or run inside
.devcontainer) -- it shells out to `idf.py`, `gen_esp32part.py`, and
`esptool`, all of which come from there.

Examples:
  # Dry run: just print the equivalent esptool command
  tools/merge_with_guest.py --slot app_slot1 --guest-bin /path/to/aquarium.bin

  # Produce a single flashable image
  tools/merge_with_guest.py --slot app_slot1 --guest-bin aquarium.bin -o combined.bin

  # Flash both directly to a connected board
  tools/merge_with_guest.py --slot app_slot1 --guest-bin aquarium.bin -p /dev/ttyUSB0
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def parse_size(text):
    """Parses gen_esp32part.py CSV size fields like '1536K', '64K', '2M', or a raw byte count."""
    text = text.strip()
    m = re.fullmatch(r"(\d+)([KM]?)", text)
    if not m:
        raise SystemExit(f"couldn't parse size '{text}' from partition table dump")
    value = int(m.group(1))
    if m.group(2) == "K":
        return value * 1024
    if m.group(2) == "M":
        return value * 1024 * 1024
    return value


def get_slot_info(idf_path, partition_table_bin, slot_label):
    gen_script = Path(idf_path) / "components" / "partition_table" / "gen_esp32part.py"
    result = subprocess.run(
        [sys.executable, str(gen_script), "-q", str(partition_table_bin)],
        capture_output=True, text=True, check=True,
    )
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(",")]
        name, offset, size = fields[0], fields[3], fields[4]
        if name == slot_label:
            return int(offset, 16), parse_size(size)
    raise SystemExit(
        f"slot '{slot_label}' not found in the built partition table -- check the name against "
        f"app_registry.c / partitions.csv (ran: gen_esp32part.py on {partition_table_bin})"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--slot", required=True, help="target app partition label, e.g. app_slot1")
    parser.add_argument("--guest-bin", required=True, type=Path, help="path to the guest app's already-built .bin")
    parser.add_argument("--build-dir", default="build", type=Path, help="launcher's idf.py build output dir (default: build)")
    parser.add_argument("--skip-build", action="store_true", help="don't run `idf.py build` first (reuse existing build/)")
    parser.add_argument("--output", "-o", type=Path, help="write a single merged image here (esptool merge_bin)")
    parser.add_argument("--port", "-p", help="flash both the launcher and the guest binary directly to this serial port")
    args = parser.parse_args()

    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit(
            "IDF_PATH is not set -- run `. $IDF_PATH/export.sh` first (or run this inside .devcontainer)."
        )

    if not args.guest_bin.is_file():
        raise SystemExit(f"guest binary not found: {args.guest_bin}")

    build_dir = args.build_dir if args.build_dir.is_absolute() else REPO_ROOT / args.build_dir

    if not args.skip_build:
        subprocess.run(["idf.py", "build"], cwd=REPO_ROOT, check=True)

    flasher_args_path = build_dir / "flasher_args.json"
    if not flasher_args_path.is_file():
        raise SystemExit(f"{flasher_args_path} not found -- build the launcher first (omit --skip-build)")
    flasher_args = json.loads(flasher_args_path.read_text())

    chip = flasher_args["extra_esptool_args"]["chip"]
    before = flasher_args["extra_esptool_args"]["before"]
    after = flasher_args["extra_esptool_args"]["after"]
    flash_mode = flasher_args["flash_settings"]["flash_mode"]
    flash_freq = flasher_args["flash_settings"]["flash_freq"]
    flash_size = flasher_args["flash_settings"]["flash_size"]

    partition_table_bin = build_dir / "partition_table" / "partition-table.bin"
    slot_offset, slot_size = get_slot_info(idf_path, partition_table_bin, args.slot)

    guest_size = args.guest_bin.stat().st_size
    if guest_size > slot_size:
        raise SystemExit(
            f"ERROR: {args.guest_bin} is {guest_size} bytes, but slot '{args.slot}' is only "
            f"{slot_size} bytes ({slot_offset:#x}..{slot_offset + slot_size:#x}).\n"
            f"Flashing this anyway would silently overwrite whatever partition comes next "
            f"(esptool has no concept of partition boundaries) -- see partitions.csv. "
            f"Either shrink the guest binary or grow the slot (partitions.csv + "
            f"CONFIG_LAUNCHER_APP_SLOT_SIZE) and rebuild the launcher."
        )

    pairs = [(int(offset_str, 16), str((build_dir / rel_path).resolve()))
             for offset_str, rel_path in flasher_args["flash_files"].items()]
    pairs.append((slot_offset, str(args.guest_bin.resolve())))
    pairs.sort()

    print(f"Slot '{args.slot}': {slot_offset:#x}..{slot_offset + slot_size:#x} ({slot_size} bytes) -- "
          f"guest binary is {guest_size} bytes ({guest_size / slot_size:.0%} full)")

    flash_common = ["--flash_mode", flash_mode, "--flash_freq", flash_freq, "--flash_size", flash_size]
    addr_file_args = []
    for offset, path in pairs:
        addr_file_args += [hex(offset), path]

    did_something = False

    if args.output:
        cmd = [sys.executable, "-m", "esptool", "--chip", chip, "merge_bin", "-o", str(args.output)] \
            + flash_common + addr_file_args
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
        print(f"\nWrote {args.output}. Flash it with:\n"
              f"  python -m esptool --chip {chip} -p PORT --before {before} --after {after} "
              f"write_flash 0x0 {args.output}")
        did_something = True

    if args.port:
        cmd = [sys.executable, "-m", "esptool", "--chip", chip, "-p", args.port,
               "--before", before, "--after", after, "write_flash"] + flash_common + addr_file_args
        print("+", " ".join(cmd))
        subprocess.run(cmd, check=True)
        did_something = True

    if not did_something:
        cmd = [sys.executable, "-m", "esptool", "--chip", chip, "-p", "PORT",
               "--before", before, "--after", after, "write_flash"] + flash_common + addr_file_args
        print("Dry run -- pass --output FILE.bin to produce a merged image, or --port PORT to flash "
              "directly. Equivalent command:\n ", " ".join(cmd))


if __name__ == "__main__":
    main()
