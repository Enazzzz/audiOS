#!/usr/bin/env python3
"""Write only the system FAT of audios.img onto a USB, leaving the data partition.

Raw-flashing the whole 64 MiB image replaces the MBR and drops partition 2.
This copies partition 1's sectors (Limine + kernel) in place so D: survives.

Usage:
  python3 tools/update_system.py /dev/sdX audios.img
  python3 tools/update_system.py --dry-run /dev/sdX audios.img

Needs root. Does not format, mkfs, or touch partition 2.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
MBR_OFF = 446


def read_part(mbr: bytes, slot: int) -> tuple[int, int, int]:
	"""Return (type, lba, sectors) for MBR slot 0-3."""
	off = MBR_OFF + slot * 16
	typ = mbr[off + 4]
	lba = struct.unpack_from("<I", mbr, off + 8)[0]
	n = struct.unpack_from("<I", mbr, off + 12)[0]
	return typ, lba, n


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("disk", help="destination whole disk (e.g. /dev/sdb)")
	ap.add_argument("image", nargs="?", default="audios.img", help="new audios.img")
	ap.add_argument("--dry-run", action="store_true")
	args = ap.parse_args()
	img = Path(args.image)
	if not img.is_file():
		print(f"missing image {img}", file=sys.stderr)
		return 1
	src = img.read_bytes()
	if len(src) < 4096 or src[510] != 0x55 or src[511] != 0xAA:
		print("image is not an MBR disk", file=sys.stderr)
		return 1
	styp, slba, ssec = read_part(src[:SECTOR], 0)
	if slba == 0 or ssec == 0:
		print("image has no partition 1", file=sys.stderr)
		return 1
	with open(args.disk, "rb") as f:
		dst_mbr = f.read(SECTOR)
	if len(dst_mbr) < SECTOR or dst_mbr[510] != 0x55:
		print("destination has no MBR — use a full flash first", file=sys.stderr)
		return 1
	dtyp, dlba, dsec = read_part(dst_mbr, 0)
	_, d2lba, d2sec = read_part(dst_mbr, 1)
	if dlba == 0 or dsec == 0:
		print("destination has no partition 1", file=sys.stderr)
		return 1
	if d2lba == 0 or d2sec == 0:
		print("no data partition on the stick; a full audios.img flash is OK", file=sys.stderr)
		print("this tool is for updating C: without wiping D:", file=sys.stderr)
		return 1
	# Never write past the existing system partition.
	n = min(ssec, dsec)
	src_off = slba * SECTOR
	dst_off = dlba * SECTOR
	payload = src[src_off : src_off + n * SECTOR]
	if len(payload) != n * SECTOR:
		print("image is shorter than partition 1", file=sys.stderr)
		return 1
	print(f"system type {styp:#x}->{dtyp:#x}  LBA {dlba}  {n} sectors  ({n * SECTOR // 1024} KiB)")
	print(f"leaving partition 2 at LBA {d2lba} ({d2sec} sectors) untouched")
	if args.dry_run:
		print("dry-run: no write")
		return 0
	with open(args.disk, "r+b") as f:
		f.seek(dst_off)
		f.write(payload)
		f.flush()
	print("wrote C: only. Reboot the FX board into the new kernel.")
	return 0


if __name__ == "__main__":
	sys.exit(main())
