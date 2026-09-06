#!/usr/bin/env python3
"""Sanity-check a 1.44 MiB Limine FAT12 floppy image."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

SECTOR = 512
SIZE = 2880 * SECTOR
PART_START = 64
HOOK_LBA = 63


def main() -> int:
	path = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.flp")
	if not path.is_file():
		print(f"missing {path}", file=sys.stderr)
		return 1
	data = path.read_bytes()
	if len(data) != SIZE:
		print(f"{path} is {len(data)} bytes, want {SIZE}", file=sys.stderr)
		return 1
	if data[510] != 0x55 or data[511] != 0xAA:
		print("boot sector missing 0xAA55", file=sys.stderr)
		return 1
	if data[0] not in (0xEB, 0xE9):
		print("boot sector is not a jump", file=sys.stderr)
		return 1
	mbr = data[:SECTOR]
	# Drive-check `cmp dl,0x80` must have been replaced with a near call.
	if mbr.find(bytes.fromhex("80 fa 80 72")) >= 0:
		print("Limine floppy reject (cmp dl,0x80) is still in the MBR", file=sys.stderr)
		return 1
	if 0xE8 not in mbr[0x50:0x70]:
		print("MBR missing near-call installer stub", file=sys.stderr)
		return 1
	hook = data[HOOK_LBA * SECTOR : (HOOK_LBA + 1) * SECTOR]
	if b"AOSI13" not in hook:
		print("INT13 EDD shim missing at LBA 63", file=sys.stderr)
		return 1
	# Partition 0: bootable, starts at LBA 64.
	boot, _, _, _, ptype, _, _, _, start, count = struct.unpack_from("<BBBBBBBBII", mbr, 0x1BE)
	if boot != 0x80 or start != PART_START or count == 0:
		print(f"bad MBR partition boot={boot:#x} start={start} count={count}", file=sys.stderr)
		return 1
	bpb = data[PART_START * SECTOR : PART_START * SECTOR + SECTOR]
	if bpb[21] not in (0xF0, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF):
		print("FAT12 BPB media descriptor missing", file=sys.stderr)
		return 1
	low = data.lower()
	if b"limine-bios.sys" not in low and b"limine" not in low:
		print("image does not contain limine-bios.sys", file=sys.stderr)
		return 1
	if b"kernel" not in low:
		print("image does not contain kernel", file=sys.stderr)
		return 1
	if b"aosboot" not in low and b"AOSBOOT" not in data:
		print("volume label AOSBOOT missing", file=sys.stderr)
		return 1
	print(
		f"{path}: 1.44 MiB MBR+FAT12, Limine + INT13 shim + kernel, "
		f"part LBA {start}"
	)
	return 0


if __name__ == "__main__":
	sys.exit(main())
