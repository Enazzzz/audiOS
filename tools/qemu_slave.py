#!/usr/bin/env python3
"""Boot audios32.flp in qemu-system-i386 and require the slave banner."""

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
from qemu_smoke import kernel_version, strip_ansi, send, wait_for  # noqa: E402

PROMPT = "slave>"


def main() -> int:
	flp = Path(sys.argv[1] if len(sys.argv) > 1 else "audios32.flp")
	if not flp.is_file():
		print(f"missing floppy: {flp}", file=sys.stderr)
		return 1
	ver = kernel_version()
	cmd = [
		"qemu-system-i386",
		"-M",
		"pc",
		"-m",
		"32M",
		"-drive",
		f"file={flp},if=floppy,format=raw",
		"-boot",
		"a",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
		"-device",
		"AC97",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	try:
		text = wait_for(master, proc, "slave>", 30.0)
		if "audiOS slave" not in text:
			print(text[-2000:], file=sys.stderr)
			print("missing audiOS slave banner", file=sys.stderr)
			return 1
		if "A7V333" not in text and "SLAVE" not in text:
			print(text[-2000:], file=sys.stderr)
			print("slave banner missing A7V333/SLAVE", file=sys.stderr)
			return 1
		send(master, "help")
		help_txt = wait_for(master, proc, "reboot", 10.0)
		if "link" not in help_txt:
			print(help_txt[-2000:], file=sys.stderr)
			return 1
		send(master, "version")
		ver_txt = wait_for(master, proc, "i386 slave", 10.0)
		if ver not in ver_txt:
			print(ver_txt[-2000:], file=sys.stderr)
			return 1
		send(master, "link")
		wait_for(master, proc, "role SLAVE", 10.0)
		send(master, "link test")
		wait_for(master, proc, "link test ok", 20.0)
		print("audios32.flp booted (32-bit slave OS)")
		return 0
	except RuntimeError as exc:
		print(exc, file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)


if __name__ == "__main__":
	raise SystemExit(main())
