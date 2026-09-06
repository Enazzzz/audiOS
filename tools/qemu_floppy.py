#!/usr/bin/env python3
"""Boot audios.flp as BIOS drive A: and require the audiOS banner."""

from __future__ import annotations

import os
import pty
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_smoke import kernel_version, strip_ansi  # noqa: E402


def main() -> int:
	flp = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.flp")
	if not flp.is_file():
		print(f"missing floppy: {flp}", file=sys.stderr)
		return 1
	cmd = [
		"qemu-system-x86_64",
		"-M",
		"pc",
		"-m",
		"128M",
		"-drive",
		f"file={flp},if=floppy,format=raw",
		"-boot",
		"a",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
	]
	ver = kernel_version()
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	deadline = time.time() + 90.0
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
			if f"audiOS {ver}" in text or "96 kHz" in text:
				print("audios.flp booted (Limine floppy chainloader)")
				return 0
		text = strip_ansi(bytes(buf))
		print(f"did not reach audiOS banner from floppy\n{text[-2000:]}", file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)


if __name__ == "__main__":
	raise SystemExit(main())
