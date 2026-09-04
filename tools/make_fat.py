#!/usr/bin/env python3
"""Create a FAT32 disk image (MBR + one partition). Never used by the kernel."""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

SECTOR = 512
PART_START = 2048  # 1 MiB
SPC = 8  # 4 KiB clusters
RESERVED = 32
NUM_FATS = 2
ROOT_CLUS = 2
LABEL = b"AUDIOS     "


def _short_name(name: str) -> bytes:
	"""Build an 8.3 directory-entry name (padded)."""
	name = name.replace(" ", "")
	if name in (".", ".."):
		raw = name.encode("ascii")
		return raw + b" " * (11 - len(raw))
	upper = name.upper()
	if "." in upper:
		stem, ext = upper.rsplit(".", 1)
	else:
		stem, ext = upper, ""
	stem = "".join(c if c.isalnum() or c in "-_" else "_" for c in stem)
	ext = "".join(c if c.isalnum() or c in "-_" else "_" for c in ext)
	if len(stem) > 8 or len(ext) > 3:
		# VFAT numeric-tail so we do not pretend limine-bios.sys is LIMINE-B.SYS.
		stem = (stem[:6] + "~1")[:8]
		ext = ext[:3]
	else:
		stem = stem[:8]
		ext = ext[:3]
	return (stem.ljust(8) + ext.ljust(3)).encode("ascii")


def _needs_lfn(name: str) -> bool:
	"""True when the name cannot be stored as a lossless 8.3 entry."""
	if name in (".", ".."):
		return False
	if "." in name:
		stem, ext = name.rsplit(".", 1)
	else:
		stem, ext = name, ""
	if len(stem) > 8 or len(ext) > 3 or stem == "" or " " in name:
		return True
	allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_~"
	return any(c.upper() not in allowed for c in stem + ext)


def _lfn_checksum(short11: bytes) -> int:
	"""VFAT checksum of the 11-byte 8.3 name."""
	s = 0
	for b in short11:
		s = (((s & 1) << 7) | (s >> 1)) + b
		s &= 0xFF
	return s


def _lfn_entries(name: str, checksum: int) -> list[bytes]:
	"""Build VFAT long-name entries (on-disk order: last fragment first)."""
	units = [ord(c) for c in name] + [0]
	nslot = (len(units) + 12) // 13
	out: list[bytes] = []
	for seq in range(nslot, 0, -1):
		chunk = units[(seq - 1) * 13 : seq * 13]
		while len(chunk) < 13:
			chunk.append(0xFFFF)
		ent = bytearray(32)
		ent[0] = seq | (0x40 if seq == nslot else 0)
		ent[11] = 0x0F
		ent[13] = checksum
		for i, ch in enumerate(chunk[:5]):
			struct.pack_into("<H", ent, 1 + i * 2, ch)
		for i, ch in enumerate(chunk[5:11]):
			struct.pack_into("<H", ent, 14 + i * 2, ch)
		for i, ch in enumerate(chunk[11:13]):
			struct.pack_into("<H", ent, 28 + i * 2, ch)
		out.append(bytes(ent))
	return out


class FatImage:
	"""In-memory FAT32 volume with a single MBR partition."""

	def __init__(self, total_bytes: int) -> None:
		if total_bytes < 8 * 1024 * 1024:
			raise ValueError("image must be at least 8 MiB")
		self.total_bytes = total_bytes
		self.part_sectors = (total_bytes // SECTOR) - PART_START
		self.data = bytearray(total_bytes)
		self._init_geometry()
		self._write_mbr()
		self._write_boot()
		self._zero_fats()
		self._mark_reserved_clusters()
		self._init_root()

	def _init_geometry(self) -> None:
		"""Compute FAT size so data clusters fill the partition."""
		# fat_sz * 512 / 4 = cluster entries. Need enough for data clusters.
		# data_secs = part - reserved - 2*fat_sz
		# clusters = data_secs / SPC
		# fat_sz >= ceil((clusters+2)*4 / 512)
		for fat_sz in range(32, 65536):
			data_secs = self.part_sectors - RESERVED - NUM_FATS * fat_sz
			if data_secs <= SPC:
				continue
			clusters = data_secs // SPC
			need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
			if fat_sz >= need:
				self.fat_sz = fat_sz
				self.clusters = clusters
				self.data_lba = PART_START + RESERVED + NUM_FATS * fat_sz
				return
		raise RuntimeError("could not size FAT")

	def _psec(self, off: int, blob: bytes) -> None:
		"""Write bytes at partition-relative sector offset."""
		self.data[(PART_START + off) * SECTOR : (PART_START + off) * SECTOR + len(blob)] = blob

	def _write_mbr(self) -> None:
		"""Protective MBR with one FAT32 LBA partition."""
		# status 0x80, type 0x0C, start PART_START, size part_sectors
		ent = struct.pack(
			"<BBBBBBBBII",
			0x80,
			0,
			0,
			0,
			0x0C,
			0,
			0,
			0,
			PART_START,
			self.part_sectors,
		)
		self.data[0x1BE : 0x1BE + 16] = ent
		self.data[510] = 0x55
		self.data[511] = 0xAA

	def _write_boot(self) -> None:
		"""FAT32 BPB, FSInfo, and backup boot sector."""
		boot = bytearray(SECTOR)
		boot[0:3] = b"\xEB\x58\x90"
		boot[3:11] = b"MSWIN4.1"
		struct.pack_into("<H", boot, 11, SECTOR)
		boot[13] = SPC
		struct.pack_into("<H", boot, 14, RESERVED)
		boot[16] = NUM_FATS
		boot[21] = 0xF8
		struct.pack_into("<H", boot, 24, 63)
		struct.pack_into("<H", boot, 26, 255)
		struct.pack_into("<I", boot, 28, PART_START)
		struct.pack_into("<I", boot, 32, self.part_sectors)
		struct.pack_into("<I", boot, 36, self.fat_sz)
		struct.pack_into("<I", boot, 44, ROOT_CLUS)
		struct.pack_into("<H", boot, 48, 1)
		struct.pack_into("<H", boot, 50, 6)
		boot[64] = 0x80
		boot[66] = 0x29
		struct.pack_into("<I", boot, 67, 0xA0D10500)
		boot[71:82] = LABEL
		boot[82:90] = b"FAT32   "
		boot[510] = 0x55
		boot[511] = 0xAA
		self.data[PART_START * SECTOR : PART_START * SECTOR + SECTOR] = boot
		# backup
		self.data[(PART_START + 6) * SECTOR : (PART_START + 7) * SECTOR] = boot
		fsinfo = bytearray(SECTOR)
		struct.pack_into("<I", fsinfo, 0, 0x41615252)
		struct.pack_into("<I", fsinfo, 484, 0x61417272)
		struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
		struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)
		fsinfo[510] = 0x55
		fsinfo[511] = 0xAA
		self.data[(PART_START + 1) * SECTOR : (PART_START + 2) * SECTOR] = fsinfo

	def _fat_off(self, n: int) -> int:
		"""Byte offset of FAT copy `n` from the start of the image."""
		return (PART_START + RESERVED + n * self.fat_sz) * SECTOR

	def _set_fat(self, clus: int, val: int) -> None:
		"""Write a 32-bit FAT entry on both FAT copies."""
		for n in range(NUM_FATS):
			off = self._fat_off(n) + clus * 4
			self.data[off : off + 4] = struct.pack("<I", val & 0x0FFFFFFF)

	def _get_fat(self, clus: int) -> int:
		"""Read a FAT entry from copy 0."""
		off = self._fat_off(0) + clus * 4
		return struct.unpack_from("<I", self.data, off)[0] & 0x0FFFFFFF

	def _zero_fats(self) -> None:
		"""Clear both FAT regions (already zero from bytearray)."""
		return

	def _mark_reserved_clusters(self) -> None:
		"""Media signature, EOC for cluster 1, EOC for root."""
		self._set_fat(0, 0x0FFFFFF8)
		self._set_fat(1, 0x0FFFFFFF)
		self._set_fat(ROOT_CLUS, 0x0FFFFFFF)

	def _clus_off(self, clus: int) -> int:
		"""Byte offset of cluster `clus`."""
		return (self.data_lba + (clus - 2) * SPC) * SECTOR

	def _init_root(self) -> None:
		"""Empty root cluster with a volume-label entry."""
		off = self._clus_off(ROOT_CLUS)
		ent = bytearray(32)
		ent[0:11] = LABEL
		ent[11] = 0x08
		self.data[off : off + 32] = ent

	def _alloc_clus(self) -> int:
		"""Allocate a zeroed cluster and mark it EOC."""
		for c in range(2, self.clusters + 2):
			if self._get_fat(c) == 0:
				self._set_fat(c, 0x0FFFFFFF)
				start = self._clus_off(c)
				self.data[start : start + SPC * SECTOR] = b"\x00" * (SPC * SECTOR)
				return c
		raise RuntimeError("FAT full")

	def _read_dir_entries(self, clus: int) -> bytearray:
		"""Concatenate the cluster chain of a directory."""
		blob = bytearray()
		seen = set()
		while 2 <= clus < 0x0FFFFFF8:
			if clus in seen:
				break
			seen.add(clus)
			start = self._clus_off(clus)
			blob.extend(self.data[start : start + SPC * SECTOR])
			clus = self._get_fat(clus)
		return blob

	def _write_dir_entries(self, first: int, blob: bytes) -> None:
		"""Write directory bytes back across the existing chain (must fit)."""
		clus = first
		pos = 0
		seen = set()
		while 2 <= clus < 0x0FFFFFF8 and pos < len(blob):
			if clus in seen:
				break
			seen.add(clus)
			chunk = SPC * SECTOR
			piece = blob[pos : pos + chunk]
			if len(piece) < chunk:
				piece = piece + b"\x00" * (chunk - len(piece))
			start = self._clus_off(clus)
			self.data[start : start + chunk] = piece
			pos += chunk
			clus = self._get_fat(clus)
		if pos < len(blob):
			raise RuntimeError("directory overflow")

	def _find_dir(self, parts: list[str]) -> int:
		"""Return the first cluster of the directory named by `parts`."""
		clus = ROOT_CLUS
		for part in parts:
			blob = self._read_dir_entries(clus)
			found = None
			want = _short_name(part)
			for i in range(0, len(blob), 32):
				ent = blob[i : i + 32]
				if ent[0] in (0x00, 0xE5) or ent[11] == 0x0F:
					continue
				if ent[0:11] == want and (ent[11] & 0x10):
					hi, lo = struct.unpack_from("<HH", ent, 20)[0], struct.unpack_from("<HH", ent, 26)[0]
					found = (hi << 16) | lo
					break
			if found is None:
				raise FileNotFoundError("/".join(parts))
			clus = found
		return clus

	def mkdir(self, path: str) -> None:
		"""Create a directory (parents must exist)."""
		path = path.strip("/").replace("\\", "/")
		if not path:
			return
		parts = path.split("/")
		parent = self._find_dir(parts[:-1])
		name = parts[-1]
		blob = self._read_dir_entries(parent)
		want = _short_name(name)
		for i in range(0, len(blob), 32):
			if blob[i : i + 11] == want:
				return
		newc = self._alloc_clus()
		dot = bytearray(64)
		dot[0:11] = _short_name(".")
		dot[11] = 0x10
		struct.pack_into("<H", dot, 20, (newc >> 16) & 0xFFFF)
		struct.pack_into("<H", dot, 26, newc & 0xFFFF)
		dot[32:43] = _short_name("..")
		dot[43] = 0x10
		struct.pack_into("<H", dot, 52, (parent >> 16) & 0xFFFF)
		struct.pack_into("<H", dot, 58, parent & 0xFFFF)
		off = self._clus_off(newc)
		self.data[off : off + 64] = dot
		self._add_dirent(parent, name, 0x10, newc, 0)

	def add_file(self, dest: str, data: bytes) -> None:
		"""Create `dest` (absolute POSIX path) with `data`."""
		dest = dest.strip("/").replace("\\", "/")
		parts = dest.split("/")
		parent = self._find_dir(parts[:-1])
		name = parts[-1]
		clus = 0
		remain = data
		first = 0
		prev = 0
		if remain:
			while True:
				c = self._alloc_clus()
				if first == 0:
					first = c
				if prev:
					self._set_fat(prev, c)
				chunk = remain[: SPC * SECTOR]
				start = self._clus_off(c)
				self.data[start : start + len(chunk)] = chunk
				remain = remain[len(chunk) :]
				prev = c
				if not remain:
					self._set_fat(c, 0x0FFFFFFF)
					break
			clus = first
		self._add_dirent(parent, name, 0x20, clus, len(data))

	def _grow_dir(self, first: int, blob: bytearray) -> bytearray:
		"""Append a cluster to a directory that ran out of slots."""
		tail = first
		seen: set[int] = set()
		while 2 <= tail < 0x0FFFFFF8:
			if tail in seen:
				break
			seen.add(tail)
			nxt = self._get_fat(tail)
			if nxt >= 0x0FFFFFF8:
				break
			tail = nxt
		newc = self._alloc_clus()
		self._set_fat(tail, newc)
		blob.extend(b"\x00" * (SPC * SECTOR))
		return blob

	def _free_slots(self, blob: bytearray, n: int) -> int:
		"""Byte offset of `n` consecutive free 32-byte directory slots."""
		need = n * 32
		for i in range(0, len(blob) - need + 1, 32):
			ok = True
			for k in range(n):
				if blob[i + k * 32] not in (0x00, 0xE5):
					ok = False
					break
			if ok:
				return i
		return -1

	def _add_dirent(self, parent: int, name: str, attr: int, clus: int, size: int) -> None:
		"""Insert an 8.3 entry, plus a VFAT LFN when the name is not 8.3."""
		short = _short_name(name)
		records: list[bytes] = []
		if _needs_lfn(name):
			records.extend(_lfn_entries(name, _lfn_checksum(short)))
		ent = bytearray(32)
		ent[0:11] = short
		ent[11] = attr
		struct.pack_into("<H", ent, 20, (clus >> 16) & 0xFFFF)
		struct.pack_into("<H", ent, 26, clus & 0xFFFF)
		struct.pack_into("<I", ent, 28, size)
		records.append(bytes(ent))
		blob = bytearray(self._read_dir_entries(parent))
		slot = self._free_slots(blob, len(records))
		if slot < 0:
			blob = self._grow_dir(parent, blob)
			slot = self._free_slots(blob, len(records))
		if slot < 0:
			raise RuntimeError("directory full")
		blob[slot : slot + 32 * len(records)] = b"".join(records)
		self._write_dir_entries(parent, blob)

	def write(self, path: Path) -> None:
		"""Atomically write the image to `path`."""
		path.write_bytes(self.data)


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("image")
	parser.add_argument("--size-mb", type=int, default=16)
	parser.add_argument("--file", action="append", default=[], help="host:dest (dest is FAT path)")
	parser.add_argument("--dir", action="append", default=[], help="directory to create")
	args = parser.parse_args()
	img = FatImage(args.size_mb * 1024 * 1024)
	for d in args.dir:
		img.mkdir(d)
	for spec in args.file:
		if ":" not in spec:
			print("expected host:dest", spec, file=sys.stderr)
			return 1
		host, dest = spec.split(":", 1)
		img.add_file(dest, Path(host).read_bytes())
	img.write(Path(args.image))
	print(f"wrote {args.image} ({args.size_mb} MiB FAT32, label AUDIOS)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
