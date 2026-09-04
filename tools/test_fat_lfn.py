#!/usr/bin/env python3
"""Check that long FAT names used by Limine are stored as VFAT LFNs."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_fat import FatImage, PART_START, ROOT_CLUS  # noqa: E402


def _decode_lfn(entries: list[bytes]) -> str:
	"""Rebuild a VFAT name from on-disk LFN fragments (last-first order)."""
	chars: list[str] = []
	for ent in reversed(entries):
		for off in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
			ch = int.from_bytes(ent[off : off + 2], "little")
			if ch in (0x0000, 0xFFFF):
				return "".join(chars)
			chars.append(chr(ch))
	return "".join(chars)


def _dir_names(img: FatImage, clus: int) -> list[str]:
	"""Collect 8.3 and LFN names from a directory cluster chain."""
	blob = img._read_dir_entries(clus)
	names: list[str] = []
	pending: list[bytes] = []
	for i in range(0, len(blob), 32):
		ent = bytes(blob[i : i + 32])
		if ent[0] in (0x00, 0xE5):
			pending = []
			continue
		if ent[11] == 0x0F:
			pending.append(ent)
			continue
		if pending:
			names.append(_decode_lfn(pending))
			pending = []
		stem = ent[0:8].decode("ascii", "replace").rstrip(" ")
		ext = ent[8:11].decode("ascii", "replace").rstrip(" ")
		names.append(f"{stem}.{ext}" if ext else stem)
	return names


def main() -> int:
	with tempfile.TemporaryDirectory() as tmp:
		img_path = Path(tmp) / "t.img"
		fat = FatImage(8 * 1024 * 1024)
		fat.mkdir("boot")
		fat.mkdir("boot/limine")
		fat.add_file("boot/limine/limine-bios.sys", b"stage3-placeholder")
		fat.add_file("boot/limine/limine.conf", b"timeout: 0\n")
		fat.add_file("limine-bios.sys", b"stage3-root")
		fat.write(img_path)
		fat2 = FatImage.__new__(FatImage)
		fat2.data = bytearray(img_path.read_bytes())
		fat2.total_bytes = len(fat2.data)
		fat2.part_sectors = (len(fat2.data) // 512) - PART_START
		fat2.grow_mb = 48
		fat2._init_geometry()
		root = _dir_names(fat2, ROOT_CLUS)
		if "limine-bios.sys" not in root:
			print("root missing LFN limine-bios.sys; have", root, file=sys.stderr)
			return 1
		# Walk /boot/limine via 8.3 directory names.
		boot = None
		blob = fat2._read_dir_entries(ROOT_CLUS)
		for i in range(0, len(blob), 32):
			ent = blob[i : i + 32]
			if ent[0:11] == b"BOOT       " and ent[11] & 0x10:
				boot = (int.from_bytes(ent[20:22], "little") << 16) | int.from_bytes(ent[26:28], "little")
		if not boot:
			print("missing boot dir", file=sys.stderr)
			return 1
		lim = None
		blob = fat2._read_dir_entries(boot)
		for i in range(0, len(blob), 32):
			ent = blob[i : i + 32]
			if ent[0:11] == b"LIMINE     " and ent[11] & 0x10:
				lim = (int.from_bytes(ent[20:22], "little") << 16) | int.from_bytes(ent[26:28], "little")
		if not lim:
			print("missing limine dir", file=sys.stderr)
			return 1
		names = _dir_names(fat2, lim)
		for need in ("limine-bios.sys", "limine.conf"):
			if need not in names:
				print(f"{need} missing LFN in /boot/limine; have {names}", file=sys.stderr)
				return 1
		if "LIMINE-B.SYS" in names:
			print("old truncated 8.3 name still used as the only name", names, file=sys.stderr)
			return 1
		# 64 MiB is the boot USB size. Cluster count must be FAT32 for Limine.
		boot = FatImage(64 * 1024 * 1024)
		if boot.clusters < 65525:
			print(
				f"boot image would be FAT16 to Limine ({boot.clusters} clusters, spc={boot.spc})",
				file=sys.stderr,
			)
			return 1
		if boot.fat_sz * 512 // 4 < 1_800_000:
			print(
				f"FAT too small to grow onto a ~1 GiB stick (fat_sz={boot.fat_sz})",
				file=sys.stderr,
			)
			return 1
	print("FAT LFN for limine-bios.sys and limine.conf ok")
	return 0


if __name__ == "__main__":
	sys.exit(main())
