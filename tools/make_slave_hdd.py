#!/usr/bin/env python3
"""Pack boot.bin + kernel.bin into a raw IDE disk image (audios32.hdd)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from make_slave_floppy import patch_boot  # noqa: E402

SECTOR = 512
DEFAULT_MIB = 16


def main() -> int:
	ap = argparse.ArgumentParser(description="Build the 32-bit slave IDE image")
	ap.add_argument("-o", "--output", default="audios32.hdd")
	ap.add_argument("--boot", required=True)
	ap.add_argument("--kernel", required=True)
	ap.add_argument("--size-mb", type=int, default=DEFAULT_MIB)
	args = ap.parse_args()
	boot = Path(args.boot).read_bytes()
	kern = Path(args.kernel).read_bytes()
	if len(boot) != 512:
		raise SystemExit(f"boot.bin must be 512 bytes, got {len(boot)}")
	if boot[-2:] != b"\x55\xaa":
		raise SystemExit("boot.bin missing 0xAA55")
	sectors = (len(kern) + 511) // 512
	nbytes = args.size_mb * 1024 * 1024
	if nbytes < 2 * 1024 * 1024:
		raise SystemExit("IDE image must be at least 2 MiB")
	if 1 + sectors >= nbytes // SECTOR:
		raise SystemExit("kernel does not fit")
	boot = bytearray(boot)
	patch_boot(boot, len(kern), 1)
	# Active type 0x06 so Award BIOS on the A7V333 lists the disk.
	part = bytearray(16)
	part[0] = 0x80
	part[1] = 0x00
	part[2] = 0x02
	part[3] = 0x00
	part[4] = 0x06
	part[5] = 0xFE
	part[6] = 0xFF
	part[7] = 0xFF
	struct.pack_into("<I", part, 8, 1)
	struct.pack_into("<I", part, 12, nbytes // SECTOR - 1)
	boot[0x1BE : 0x1BE + 16] = part
	img = bytearray(nbytes)
	img[0:512] = boot
	img[512 : 512 + len(kern)] = kern
	Path(args.output).write_bytes(img)
	print(f"wrote {args.output}  {args.size_mb} MiB  kernel {len(kern)} bytes  {sectors} sectors")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
