#!/usr/bin/env python3
"""Pack boot.bin + kernel.bin into a 1.44 MB floppy image (audios32.flp)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

FLOPPY = 2880 * 512
KERNEL_LBA_OFF = 0x1AC
KERNEL_NSEC_OFF = 0x1B0


def patch_boot(boot: bytearray, kernel_len: int, kernel_lba: int = 1) -> None:
	"""Store kernel LBA and sector count in the unified slave MBR."""
	sectors = (kernel_len + 511) // 512
	struct.pack_into("<I", boot, KERNEL_LBA_OFF, kernel_lba)
	struct.pack_into("<H", boot, KERNEL_NSEC_OFF, sectors)


def main() -> int:
	ap = argparse.ArgumentParser(description="Build the 32-bit slave floppy")
	ap.add_argument("-o", "--output", default="audios32.flp")
	ap.add_argument("--boot", required=True)
	ap.add_argument("--kernel", required=True)
	args = ap.parse_args()
	boot = Path(args.boot).read_bytes()
	kern = Path(args.kernel).read_bytes()
	if len(boot) != 512:
		raise SystemExit(f"boot.bin must be 512 bytes, got {len(boot)}")
	if boot[-2:] != b"\x55\xaa":
		raise SystemExit("boot.bin missing 0xAA55")
	sectors = (len(kern) + 511) // 512
	if 1 + sectors > 2880:
		raise SystemExit(f"kernel too large for 1.44 MB ({sectors} sectors)")
	boot = bytearray(boot)
	patch_boot(boot, len(kern), 1)
	img = bytearray(FLOPPY)
	img[0:512] = boot
	img[512 : 512 + len(kern)] = kern
	Path(args.output).write_bytes(img)
	print(f"wrote {args.output}  kernel {len(kern)} bytes  {sectors} sectors")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
