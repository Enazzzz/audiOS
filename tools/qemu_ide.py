#!/usr/bin/env python3
"""FX kernel formats a blank IDE HDD and installs the A7V333 slave OS."""

from __future__ import annotations

import os
import pty
import subprocess
import sys
import termios
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_slave import boot_and_test  # noqa: E402
from qemu_smoke import PROMPT, send, wait_for  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
	iso = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.iso")
	fs_img = Path("audios-fs.img")
	hdd = Path("audios-ide-scratch.hdd")
	if not iso.is_file() or not fs_img.is_file():
		print("need audios.iso and audios-fs.img", file=sys.stderr)
		return 1
	scratch = bytearray(16 * 1024 * 1024)
	# Junk in the boot area and at the tail — `ide format` must clear it.
	scratch[100 * 512 : 101 * 512] = b"\xaa" * 512
	scratch[-512:] = b"\xbb" * 512
	hdd.write_bytes(scratch)
	# pc + if=ide lands the scratch disk on legacy 0x1F0 (PIIX). q35 is AHCI.
	cmd = [
		"qemu-system-x86_64",
		"-M",
		"pc",
		"-m",
		"128M",
		"-cdrom",
		str(iso),
		"-boot",
		"d",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
		"-drive",
		f"if=none,id=stick,file={fs_img},format=raw,cache=directsync",
		"-device",
		"usb-ehci,id=ehci",
		"-device",
		"usb-storage,bus=ehci.0,drive=stick",
		"-drive",
		f"file={hdd},if=ide,format=raw,cache=directsync",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	try:
		wait_for(master, proc, PROMPT, 45.0)
		send(master, "ide")
		text = wait_for(master, proc, PROMPT, 15.0)
		if "0x1f0" not in text.lower() and "0x1F0" not in text:
			# Status prints 0x1f0 via %x.
			if "master" not in text.lower():
				raise RuntimeError(f"ide did not see an IDE HDD\n{text[-2000:]}")
		if "slave.bin" not in text:
			raise RuntimeError(f"C:/boot/slave.bin missing on the USB image\n{text[-2000:]}")
		send(master, "ide format")
		text = wait_for(master, proc, "IDE HDD has audiOS slave", 60.0)
		if "refusing" in text.lower():
			raise RuntimeError(f"ide format refused the scratch HDD\n{text[-2000:]}")
		if "format ok" not in text:
			raise RuntimeError(f"ide format did not format the HDD\n{text[-2000:]}")
		if PROMPT not in text:
			wait_for(master, proc, PROMPT, 10.0)
		print("FX ide format formatted the IDE HDD and installed the A7V333 slave OS")
	except (RuntimeError, TimeoutError) as exc:
		print(exc, file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)
	mbr = hdd.read_bytes()[:512]
	if mbr[510:512] != b"\x55\xaa":
		print("scratch HDD has no 0xAA55 after ide format", file=sys.stderr)
		return 1
	if mbr[0x1BE] != 0x80:
		print("scratch HDD missing active partition", file=sys.stderr)
		return 1
	mid = hdd.read_bytes()[100 * 512 : 101 * 512]
	if mid != b"\x00" * 512:
		print("ide format left junk at LBA 100 (did not format the HDD)", file=sys.stderr)
		return 1
	if hdd.read_bytes()[-512:] != b"\x00" * 512:
		print("ide format left junk at the end of the HDD", file=sys.stderr)
		return 1
	return boot_and_test(hdd)


if __name__ == "__main__":
	raise SystemExit(main())
