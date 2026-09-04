#!/usr/bin/env python3
"""Boot audios.img with i8042 off and type through a QEMU USB keyboard on OHCI."""

from __future__ import annotations

import os
import pty
import select
import socket
import subprocess
import sys
import termios
import time
from pathlib import Path

ANSI = __import__("re").compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(data: bytes) -> str:
	"""Remove colour sequences so assertions can match plain text."""
	return ANSI.sub(b"", data).decode("utf-8", errors="replace")


def wait_needle(master: int, proc: subprocess.Popen[bytes], needle: str, deadline: float, buf: bytearray) -> str:
	"""Read the serial PTY until `needle` appears or time runs out."""
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
		if needle in text:
			return text
	raise TimeoutError(f"timed out waiting for {needle!r}\n{strip_ansi(bytes(buf))[-2500:]}")


def monitor_sendkeys(sock_path: Path, keys: list[str]) -> None:
	"""Inject qcodes (goes to the USB kbd when i8042 is off)."""
	deadline = time.time() + 5.0
	while time.time() < deadline:
		if sock_path.exists():
			break
		time.sleep(0.05)
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.settimeout(5)
	s.connect(str(sock_path))
	time.sleep(0.3)
	try:
		s.recv(4096)
	except OSError:
		pass
	for key in keys:
		s.sendall(f"sendkey {key}\n".encode("ascii"))
		time.sleep(0.08)
	time.sleep(0.3)
	s.close()


def main() -> int:
	img = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.img")
	if not img.is_file():
		print(f"missing image: {img}", file=sys.stderr)
		return 1
	mon = Path("/tmp/audios-usbhid.sock")
	try:
		mon.unlink()
	except FileNotFoundError:
		pass
	cmd = [
		"qemu-system-x86_64",
		"-M",
		"pc,i8042=off",
		"-m",
		"128M",
		"-drive",
		f"file={img},format=raw,if=ide",
		"-boot",
		"c",
		"-device",
		"pci-ohci,id=ohci",
		"-device",
		"usb-kbd,bus=ohci.0",
		"-serial",
		"stdio",
		"-display",
		"none",
		"-no-reboot",
		"-vga",
		"std",
		"-monitor",
		f"unix:{mon},server,nowait",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	buf = bytearray()
	try:
		text = wait_needle(master, proc, "audiOS>", time.time() + 40.0, buf)
		if "HID keyboard" not in text:
			print(f"OHCI HID not enumerated\n{text[-2000:]}", file=sys.stderr)
			return 1
		monitor_sendkeys(mon, ["h", "e", "l", "p", "ret"])
		wait_needle(master, proc, "audiOS commands", time.time() + 12.0, buf)
		print("audios.img USB HID keyboard accepted help")
		return 0
	except Exception as exc:  # noqa: BLE001
		print(exc, file=sys.stderr)
		return 1
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)
		try:
			mon.unlink()
		except FileNotFoundError:
			pass


if __name__ == "__main__":
	sys.exit(main())
