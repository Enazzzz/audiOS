#!/usr/bin/env python3
"""Build a 1.44 MiB floppy with an MBR, Limine BIOS stages, and the kernel.

Old BIOS boxes that will not boot USB-HDD still boot drive A:. Layout:

	LBA 0     MBR + Limine stage 1 (after `limine bios-install`)
	LBA 1..63 Limine stage 2 gap
	LBA 64+   FAT12 partition: limine-bios.sys, limine.conf, boot/kernel

The kernel is on the floppy so boot does not depend on INT13 USB. Menu
entries also chainload a USB volume labeled AUDIOS.

Limine refuses BIOS floppies (DL<0x80, no EDD). After `limine bios-install`
we plant an INT13 AH=41/42/48 shim in gap LBA 63 and a 19-byte MBR stub
that loads it to 0x600. Stage 2 then sees the disk as removable drive 0xEF.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_fat import _lfn_checksum, _lfn_entries, _needs_lfn, _short_name  # noqa: E402

SECTOR = 512
SPT = 18
HEADS = 2
CYLS = 80
TOTSEC = SPT * HEADS * CYLS  # 2880
PART_START = 64  # 32 KiB gap so Limine stage 2 fits before the FAT
SPC = 1
RESERVED = 1
NFATS = 2
ROOT_ENT = 224
FATSZ = 9
MEDIA = 0xF8
LABEL = b"AOSBOOT    "
FSTYPE = b"FAT12   "
ROOT_SECS = (ROOT_ENT * 32 + SECTOR - 1) // SECTOR  # 14
PART_SECS = TOTSEC - PART_START
DATA_REL = RESERVED + NFATS * FATSZ + ROOT_SECS  # 33
CLUSTERS = PART_SECS - DATA_REL
EOC = 0xFFF


def _lba_chs(lba: int) -> tuple[int, int, int]:
	"""Pack an LBA into MBR CHS bytes."""
	cyl = lba // (SPT * HEADS)
	rest = lba % (SPT * HEADS)
	head = rest // SPT
	sect = rest % SPT + 1
	if cyl > 1023:
		return (255, 255, 255)
	return (head, (sect & 0x3F) | ((cyl >> 8) << 6), cyl & 0xFF)


def _set_fat12(fat: bytearray, clus: int, val: int) -> None:
	"""Pack a 12-bit FAT entry."""
	val &= 0xFFF
	off = (clus * 3) // 2
	if clus & 1:
		fat[off] = (fat[off] & 0x0F) | ((val & 0x0F) << 4)
		fat[off + 1] = (val >> 4) & 0xFF
	else:
		fat[off] = val & 0xFF
		fat[off + 1] = (fat[off + 1] & 0xF0) | ((val >> 8) & 0x0F)


def _get_fat12(fat: bytearray, clus: int) -> int:
	"""Unpack a 12-bit FAT entry."""
	off = (clus * 3) // 2
	if clus & 1:
		return (fat[off] >> 4) | (fat[off + 1] << 4)
	return fat[off] | ((fat[off + 1] & 0x0F) << 8)


class Fat12Floppy:
	"""1.44 MiB image: MBR + one FAT12 partition (floppy geometry)."""

	def __init__(self) -> None:
		self.data = bytearray(TOTSEC * SECTOR)
		self.fat = bytearray(FATSZ * SECTOR)
		_set_fat12(self.fat, 0, 0xF00 | MEDIA)
		_set_fat12(self.fat, 1, EOC)
		self.next_clus = 2
		self._write_mbr()
		self._write_bpb()
		self._write_fats()
		self._add_label()

	def _part_off(self, rel_sec: int) -> int:
		"""Byte offset of partition-relative sector `rel_sec`."""
		return (PART_START + rel_sec) * SECTOR

	def _write_mbr(self) -> None:
		"""One bootable FAT12 partition starting after the Limine gap."""
		h0, s0, c0 = _lba_chs(PART_START)
		h1, s1, c1 = _lba_chs(TOTSEC - 1)
		ent = struct.pack(
			"<BBBBBBBBII",
			0x80,
			h0,
			s0,
			c0,
			0x01,  # FAT12
			h1,
			s1,
			c1,
			PART_START,
			PART_SECS,
		)
		self.data[0x1BE : 0x1BE + 16] = ent
		self.data[510] = 0x55
		self.data[511] = 0xAA

	def _write_bpb(self) -> None:
		"""FAT12 BIOS Parameter Block at the start of the partition."""
		b = bytearray(SECTOR)
		b[0:3] = b"\xeb\x3c\x90"
		b[3:11] = b"MSDOS5.0"
		struct.pack_into("<H", b, 11, SECTOR)
		b[13] = SPC
		struct.pack_into("<H", b, 14, RESERVED)
		b[16] = NFATS
		struct.pack_into("<H", b, 17, ROOT_ENT)
		struct.pack_into("<H", b, 19, PART_SECS if PART_SECS < 65536 else 0)
		b[21] = MEDIA
		struct.pack_into("<H", b, 22, FATSZ)
		struct.pack_into("<H", b, 24, SPT)
		struct.pack_into("<H", b, 26, HEADS)
		struct.pack_into("<I", b, 28, PART_START)
		struct.pack_into("<I", b, 32, 0 if PART_SECS < 65536 else PART_SECS)
		b[36] = 0x00
		b[37] = 0x00
		b[38] = 0x29
		struct.pack_into("<I", b, 39, 0xA05B0510)
		b[43:54] = LABEL
		b[54:62] = FSTYPE
		b[510] = 0x55
		b[511] = 0xAA
		off = self._part_off(0)
		self.data[off : off + SECTOR] = b

	def _write_fats(self) -> None:
		"""Mirror both FAT copies."""
		for n in range(NFATS):
			off = self._part_off(RESERVED + n * FATSZ)
			self.data[off : off + len(self.fat)] = self.fat

	def _root_off(self) -> int:
		"""Byte offset of the fixed root directory."""
		return self._part_off(RESERVED + NFATS * FATSZ)

	def _add_label(self) -> None:
		"""Volume label as the first root entry."""
		ent = bytearray(32)
		ent[0:11] = LABEL
		ent[11] = 0x08
		off = self._root_off()
		self.data[off : off + 32] = ent

	def _clus_off(self, clus: int) -> int:
		"""Byte offset of data cluster `clus` (2-based)."""
		return self._part_off(DATA_REL + (clus - 2) * SPC)

	def _alloc_clus(self) -> int:
		"""Next free cluster, marked EOC."""
		c = self.next_clus
		if c >= 2 + CLUSTERS:
			raise RuntimeError("floppy full")
		_set_fat12(self.fat, c, EOC)
		self.next_clus = c + 1
		return c

	def _root_blob(self) -> bytearray:
		"""Copy of the fixed root directory."""
		off = self._root_off()
		return bytearray(self.data[off : off + ROOT_SECS * SECTOR])

	def _write_root(self, blob: bytes) -> None:
		"""Write the root directory back."""
		off = self._root_off()
		if len(blob) > ROOT_SECS * SECTOR:
			raise RuntimeError("root directory full")
		self.data[off : off + len(blob)] = blob

	def _dir_blob(self, clus: int) -> bytearray:
		"""Read a subdirectory cluster chain (or root when clus==0)."""
		if clus == 0:
			return self._root_blob()
		blob = bytearray()
		seen: set[int] = set()
		while 2 <= clus < 0xFF8:
			if clus in seen:
				break
			seen.add(clus)
			start = self._clus_off(clus)
			blob.extend(self.data[start : start + SPC * SECTOR])
			clus = _get_fat12(self.fat, clus)
		return blob

	def _write_dir(self, first: int, blob: bytes) -> None:
		"""Write a directory back to root or a cluster chain."""
		if first == 0:
			self._write_root(blob)
			return
		clus = first
		pos = 0
		while 2 <= clus < 0xFF8 and pos < len(blob):
			chunk = SPC * SECTOR
			piece = blob[pos : pos + chunk]
			if len(piece) < chunk:
				piece = piece + b"\x00" * (chunk - len(piece))
			start = self._clus_off(clus)
			self.data[start : start + chunk] = piece
			pos += chunk
			clus = _get_fat12(self.fat, clus)

	def _free_slots(self, blob: bytearray, n: int) -> int:
		"""Byte offset of `n` consecutive free 32-byte slots."""
		need = n * 32
		for i in range(0, max(0, len(blob) - need + 1), 32):
			if all(blob[i + k * 32] in (0x00, 0xE5) for k in range(n)):
				return i
		return -1

	def _add_dirent(self, parent: int, name: str, attr: int, clus: int, size: int) -> None:
		"""Insert 8.3 (+ LFN) into directory `parent` (0 = root)."""
		short = _short_name(name)
		records: list[bytes] = []
		if _needs_lfn(name):
			records.extend(_lfn_entries(name, _lfn_checksum(short)))
		ent = bytearray(32)
		ent[0:11] = short
		ent[11] = attr
		struct.pack_into("<H", ent, 26, clus & 0xFFFF)
		struct.pack_into("<I", ent, 28, size)
		records.append(bytes(ent))
		blob = self._dir_blob(parent)
		slot = self._free_slots(blob, len(records))
		if slot < 0:
			raise RuntimeError(f"directory full adding {name}")
		blob[slot : slot + 32 * len(records)] = b"".join(records)
		self._write_dir(parent, blob)

	def _find_dir(self, parts: list[str]) -> int:
		"""Cluster of the directory named by `parts` (0 = root)."""
		clus = 0
		for part in parts:
			blob = self._dir_blob(clus)
			want = _short_name(part)
			found = None
			for i in range(0, len(blob), 32):
				ent = blob[i : i + 32]
				if ent[0] in (0x00, 0xE5) or ent[11] == 0x0F:
					continue
				if ent[0:11] == want and (ent[11] & 0x10):
					found = struct.unpack_from("<H", ent, 26)[0]
					break
			if found is None:
				raise FileNotFoundError("/".join(parts))
			clus = found
		return clus

	def mkdir(self, path: str) -> None:
		"""Create a directory; parents must exist."""
		path = path.strip("/").replace("\\", "/")
		if not path:
			return
		parts = path.split("/")
		parent = self._find_dir(parts[:-1])
		name = parts[-1]
		blob = self._dir_blob(parent)
		want = _short_name(name)
		for i in range(0, len(blob), 32):
			if blob[i : i + 11] == want:
				return
		newc = self._alloc_clus()
		dot = bytearray(64)
		dot[0:11] = _short_name(".")
		dot[11] = 0x10
		struct.pack_into("<H", dot, 26, newc)
		dot[32:43] = _short_name("..")
		dot[43] = 0x10
		struct.pack_into("<H", dot, 58, parent)
		off = self._clus_off(newc)
		self.data[off : off + 64] = dot
		self._add_dirent(parent, name, 0x10, newc, 0)

	def add_file(self, dest: str, data: bytes) -> None:
		"""Create `dest` with `data`."""
		dest = dest.strip("/").replace("\\", "/")
		parts = dest.split("/")
		parent = self._find_dir(parts[:-1])
		name = parts[-1]
		first = 0
		prev = 0
		remain = data
		if remain:
			while True:
				c = self._alloc_clus()
				if first == 0:
					first = c
				if prev:
					_set_fat12(self.fat, prev, c)
				chunk = remain[: SPC * SECTOR]
				start = self._clus_off(c)
				self.data[start : start + len(chunk)] = chunk
				remain = remain[len(chunk) :]
				prev = c
				if not remain:
					_set_fat12(self.fat, c, EOC)
					break
		self._add_dirent(parent, name, 0x20, first, len(data))
		self._write_fats()

	def save(self, path: Path) -> None:
		"""Flush FATs and write the 1.44 MiB image."""
		self._write_fats()
		path.write_bytes(self.data)


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("image", help="output path (audios.flp)")
	ap.add_argument("--kernel", required=True, type=Path)
	ap.add_argument("--limine-sys", required=True, type=Path)
	ap.add_argument("--limine-conf", required=True, type=Path)
	ap.add_argument("--limine-tool", required=True, type=Path, help="host `limine` installer")
	args = ap.parse_args()

	for p in (args.kernel, args.limine_sys, args.limine_conf, args.limine_tool):
		if not p.is_file():
			print(f"missing {p}", file=sys.stderr)
			return 1

	kernel = args.kernel.read_bytes()
	sysb = args.limine_sys.read_bytes()
	conf = args.limine_conf.read_bytes()
	need = len(kernel) + len(sysb) + len(conf) + 64 * 1024
	cap = CLUSTERS * SPC * SECTOR
	if need > cap:
		print(f"floppy too small: files {need} bytes, data {cap}", file=sys.stderr)
		return 1

	img = Fat12Floppy()
	img.mkdir("boot")
	img.add_file("limine-bios.sys", sysb)
	img.add_file("limine.conf", conf)
	img.add_file("boot/kernel", kernel)
	out = Path(args.image)
	img.save(out)
	r = subprocess.run([str(args.limine_tool), "bios-install", str(out)], check=False)
	if r.returncode != 0:
		print("limine bios-install failed", file=sys.stderr)
		return 1
	hook = Path(__file__).with_name("limine_int13_hook.asm")
	if _patch_limine_mbr_for_floppy(out, hook) != 0:
		return 1
	print(f"wrote {out} ({TOTSEC * SECTOR} bytes, MBR+FAT12, Limine BIOS + INT13 EDD shim)")
	return 0


# Gap sector that holds the INT13 EDD shim (after Limine stage 2, before FAT).
HOOK_LBA = 63


def _assemble_hook(asm_path: Path) -> bytes:
	"""Assemble the INT13 shim with nasm (CI installs it)."""
	bin_path = asm_path.with_suffix(".bin")
	r = subprocess.run(
		["nasm", "-f", "bin", "-o", str(bin_path), str(asm_path)],
		check=False,
		capture_output=True,
		text=True,
	)
	if r.returncode != 0:
		print(r.stderr or r.stdout or "nasm failed", file=sys.stderr)
		raise RuntimeError("nasm failed (install nasm to build audios.flp)")
	data = bin_path.read_bytes()
	try:
		bin_path.unlink()
	except OSError:
		pass
	if not data or len(data) > SECTOR:
		raise RuntimeError(f"INT13 hook is {len(data)} bytes, need 1..{SECTOR}")
	if b"AOSI13" not in data:
		raise RuntimeError("INT13 hook missing AOSI13 signature")
	return data


def _patch_limine_mbr_for_floppy(path: Path, hook_asm: Path) -> int:
	"""Let Limine treat a BIOS floppy as an EDD drive (alias 0xEF).

	Limine stage 1 rejects DL<0x80 and requires INT13 AH=41/42/48. Stage 2
	only scans 0x80–0xEF. The shim at 0x600 emulates those calls via CHS
	AH=02 on the real floppy, for both the boot DL and alias 0xEF.
	"""
	hook = _assemble_hook(hook_asm)
	blob = bytearray(path.read_bytes())
	if len(blob) != TOTSEC * SECTOR:
		print(f"{path} is {len(blob)} bytes, not a 1.44 MiB floppy", file=sys.stderr)
		return 1
	# Stage 2 occupies LBA 1 until bootloader_img size; LBA 63 is free.
	off = HOOK_LBA * SECTOR
	if any(blob[off : off + SECTOR]):
		print(f"gap LBA {HOOK_LBA} is not empty; Limine stage 2 grew?", file=sys.stderr)
		return 1
	blob[off : off + len(hook)] = hook

	mbr = blob[:SECTOR]
	# cmp dl,0x80 / jb ; cmp dl,0x8f / ja  (10 bytes) — skip floppy reject.
	drv = bytes.fromhex("80 fa 80")
	i = mbr.find(drv)
	if i < 0 or i + 10 > 0xDA:
		print("Limine drive check not found", file=sys.stderr)
		return 1
	# 19-byte installer in the zero pad before the BIOS timestamp at 0xDA.
	zeros_at = None
	for z in range(0xC0, 0xDA - 18):
		if mbr[z : z + 19] == b"\x00" * 19:
			zeros_at = z
			break
	if zeros_at is None:
		print("no 19-byte hole in the MBR for the INT13 installer", file=sys.stderr)
		return 1
	cyl = HOOK_LBA // (SPT * HEADS)
	rest = HOOK_LBA % (SPT * HEADS)
	head = rest // SPT
	sect = rest % SPT + 1
	cx = ((cyl & 0xFF) << 8) | (sect & 0x3F) | (((cyl >> 8) & 3) << 6)
	# call install is at installer+14; rel16 to 0x600.
	call_at = zeros_at + 14
	rel_hook = (0x600 - (call_at + 3)) & 0xFFFF
	installer = bytearray(
		b"\x60"  # pusha
		+ b"\xb8\x01\x02"  # mov ax, 0x0201
		+ b"\xbb\x00\x06"  # mov bx, 0x0600
		+ bytes([0xB9, cx & 0xFF, (cx >> 8) & 0xFF])  # mov cx, CHS
		+ bytes([0xB6, head & 0xFF])  # mov dh, head
		+ b"\xcd\x13"  # int 0x13
		+ bytes([0xE8, rel_hook & 0xFF, (rel_hook >> 8) & 0xFF])  # call 0x600
		+ b"\x61\xc3"  # popa; ret
	)
	if len(installer) != 19:
		print(f"installer is {len(installer)} bytes, want 19", file=sys.stderr)
		return 1
	mbr[zeros_at : zeros_at + 19] = installer
	rel_inst = (zeros_at - (i + 3)) & 0xFFFF
	mbr[i : i + 10] = (
		bytes([0xE8, rel_inst & 0xFF, (rel_inst >> 8) & 0xFF])
		+ b"\xb2\xef"  # mov dl, 0xEF — stage 2 only indexes 0x80–0xEF
		+ b"\x90" * 5
	)
	blob[:SECTOR] = mbr
	path.write_bytes(blob)
	print(
		f"patched Limine MBR: INT13 EDD shim at LBA {HOOK_LBA} "
		f"(CHS {cyl}/{head}/{sect}), install at {zeros_at:#x}"
	)
	return 0


if __name__ == "__main__":
	sys.exit(main())
