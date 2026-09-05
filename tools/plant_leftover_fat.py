#!/usr/bin/env python3
"""Plant a FAT32 volume in leftover USB space *without* an MBR slot.

This matches the FX-board case: a 64 MiB system flash rewrites the MBR and
drops partition 2, but the old D: FAT32 is still sitting after C:. The kernel
must mount that leftover (and restore the 0x0C slot), not skip it.
"""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_fat import PART_START, SECTOR, FatImage  # noqa: E402

ALIGN = 2048


def leftover_start(img: bytes) -> tuple[int, int]:
	"""Return (aligned leftover LBA, leftover sector count) from an MBR image."""
	if len(img) < SECTOR or img[510] != 0x55 or img[511] != 0xAA:
		raise RuntimeError("image is not MBR")
	max_end = 1
	for i in range(4):
		ent = 0x1BE + i * 16
		typ = img[ent + 4]
		start, count = struct.unpack_from("<II", img, ent + 8)
		if typ == 0 or count == 0:
			continue
		end = start + count
		if end > max_end:
			max_end = end
	sectors = len(img) // SECTOR
	start = ((max_end + ALIGN - 1) // ALIGN) * ALIGN
	if start < max_end:
		start += ALIGN
	if start >= sectors:
		raise RuntimeError("no leftover sectors")
	return start, sectors - start


def plant(path: Path) -> None:
	"""Write a complete FAT32 at the leftover LBA; leave MBR slot 2 empty."""
	raw = bytearray(path.read_bytes())
	start, nsec = leftover_start(bytes(raw))
	if nsec < 32768:
		print(f"{path}: leftover {nsec} sectors < 16 MiB; skip")
		return
	# FatImage wants a whole-disk size including a dummy 1 MiB prefix.
	tmp = FatImage((nsec + PART_START) * SECTOR)
	payload = bytearray(tmp.data[PART_START * SECTOR :])
	if len(payload) != nsec * SECTOR:
		raise RuntimeError("planted FAT size mismatch")
	# Hidden sectors = the LBA we actually live at (harmless if ignored).
	struct.pack_into("<I", payload, 28, start)
	struct.pack_into("<I", payload, 32, nsec)
	# Volume label DATA so D: is distinguishable from C: AUDIOS.
	payload[71:82] = b"DATA       "
	off = start * SECTOR
	raw[off : off + len(payload)] = payload
	# Do not write an MBR type — that is the bug we are reproducing.
	path.write_bytes(raw)
	print(f"{path}: planted leftover FAT32 at LBA {start} ({nsec} sectors), MBR slot empty")


def main() -> int:
	path = Path(sys.argv[1] if len(sys.argv) > 1 else "audios-fs.img")
	if not path.is_file():
		print(f"missing {path}", file=sys.stderr)
		return 1
	plant(path)
	return 0


if __name__ == "__main__":
	sys.exit(main())
