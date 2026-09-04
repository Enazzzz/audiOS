#!/usr/bin/env python3
"""Boot audios.img with i8042 off and type through a QEMU USB keyboard on OHCI."""

from __future__ import annotations

import json
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


def qmp_type(sock_path: Path, keys: list[str]) -> None:
	"""Send key down/up through QMP so QEMU delivers them to usb-kbd."""
	deadline = time.time() + 5.0
	while time.time() < deadline:
		if sock_path.exists():
			break
		time.sleep(0.05)
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.settimeout(8)
	s.connect(str(sock_path))

	def recv_json() -> dict:
		buf = b""
		while b"\n" not in buf:
			chunk = s.recv(4096)
			if not chunk:
				break
			buf += chunk
		line = buf.split(b"\n", 1)[0]
		return json.loads(line.decode("utf-8", errors="replace") or "{}")

	def send(obj: dict) -> dict:
		s.sendall((json.dumps(obj) + "\n").encode("ascii"))
		rep = recv_json()
		if "error" in rep:
			print("qmp error", obj, rep, file=sys.stderr)
		return rep

	recv_json()
	send({"execute": "qmp_capabilities"})
	for key in keys:
		for down in (True, False):
			send({
				"execute": "input-send-event",
				"arguments": {
					"events": [{
						"type": "key",
						"data": {
							"down": down,
							"key": {"type": "qcode", "data": key},
						},
					}],
				},
			})
		time.sleep(0.05)
	time.sleep(0.2)
	s.close()


def main() -> int:
	img = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.img")
	if not img.is_file():
		print(f"missing image: {img}", file=sys.stderr)
		return 1
	qmp = Path("/tmp/audios-usbhid.qmp")
	try:
		qmp.unlink()
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
		"-qmp",
		f"unix:{qmp},server,nowait",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	buf = bytearray()
	try:
		wait_needle(master, proc, "HID keyboard", time.time() + 40.0, buf)
		# Allow the periodic list to run before injecting.
		time.sleep(0.5)
		qmp_type(qmp, ["h", "e", "l", "p", "ret"])
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
			qmp.unlink()
		except FileNotFoundError:
			pass


if __name__ == "__main__":
	sys.exit(main())
