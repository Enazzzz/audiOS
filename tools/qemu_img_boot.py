#!/usr/bin/env python3
"""Boot audios.img as a BIOS disk and require the kernel banner (not Limine stage 3)."""

from __future__ import annotations

import os
import pty
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

ANSI = __import__("re").compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(data: bytes) -> str:
	"""Remove colour sequences so assertions can match plain text."""
	return ANSI.sub(b"", data).decode("utf-8", errors="replace")


def main() -> int:
	img = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.img")
	if not img.is_file():
		print(f"missing image: {img}", file=sys.stderr)
		return 1
	cmd = [
		"qemu-system-x86_64",
		"-M",
		"pc",
		"-m",
		"128M",
		"-drive",
		f"file={img},format=raw,if=ide",
		"-boot",
		"c",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	deadline = time.time() + 40.0
	buf = bytearray()
	try:
		while time.time() < deadline:
			if proc.poll() is not None:
				break
			r, _, _ = select.select([master], [], [], 0.1)
			if not r:
				continue
			chunk = os.read(master, 4096)
			if not chunk:
				continue
			buf.extend(chunk)
			text = strip_ansi(bytes(buf))
			if "Stage 3 file not found" in text or "Failed to load stage 3" in text:
				print(text[-2000:], file=sys.stderr)
				return 1
			if "audiOS" in text:
				print("audios.img booted (Limine found stage 3)")
				return 0
		text = strip_ansi(bytes(buf))
		print(f"did not reach audiOS banner\n{text[-2000:]}", file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)


if __name__ == "__main__":
	try:
		sys.exit(main())
	except Exception as exc:  # noqa: BLE001
		print(exc, file=sys.stderr)
		sys.exit(1)
