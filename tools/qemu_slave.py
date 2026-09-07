#!/usr/bin/env python3
"""Boot the 32-bit slave OS from a floppy or an IDE image and require link test."""

from __future__ import annotations

import os
import pty
import subprocess
import sys
import termios
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_smoke import kernel_version, send, wait_for  # noqa: E402

FLOPPY_BYTES = 1474560
PROMPT = "slave>"


def qemu_cmd(image: Path) -> list[str]:
	"""Floppy images stay on A:; anything else is a BIOS HDD."""
	cmd = [
		"qemu-system-i386",
		"-M",
		"pc",
		"-m",
		"32M",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
		"-device",
		"AC97",
	]
	if image.stat().st_size == FLOPPY_BYTES:
		cmd += [
			"-drive",
			f"file={image},if=floppy,format=raw",
			"-boot",
			"a",
		]
	else:
		cmd += [
			"-drive",
			f"file={image},if=ide,format=raw",
			"-boot",
			"c",
		]
	return cmd


def boot_and_test(image: Path) -> int:
	"""Require banner, help, version, SLAVE role, and `link test ok`."""
	ver = kernel_version()
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(
		qemu_cmd(image), stdin=slave, stdout=slave, stderr=slave, close_fds=True
	)
	os.close(slave)
	kind = "floppy" if image.stat().st_size == FLOPPY_BYTES else "IDE"
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
		print(f"{image.name} booted (32-bit slave OS, {kind})")
		return 0
	except (RuntimeError, TimeoutError) as exc:
		print(exc, file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)


def main() -> int:
	image = Path(sys.argv[1] if len(sys.argv) > 1 else "audios32.flp")
	if not image.is_file():
		print(f"missing image: {image}", file=sys.stderr)
		return 1
	return boot_and_test(image)


if __name__ == "__main__":
	raise SystemExit(main())
