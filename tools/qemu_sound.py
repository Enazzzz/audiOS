#!/usr/bin/env python3
"""Capture one oscillator (or WAV play) at a time and judge the PCM."""

from __future__ import annotations

import os
import pty
import subprocess
import sys
import termios
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze_sound import judge  # noqa: E402
from qemu_smoke import PROMPT, drain, send, wait_for  # noqa: E402


def boot(iso: Path, fs_img: Path, capture: Path) -> tuple[int, subprocess.Popen[bytes]]:
	"""QEMU with HDA WAV capture and the USB FAT stick."""
	try:
		capture.unlink()
	except FileNotFoundError:
		pass
	cmd = [
		"qemu-system-x86_64",
		"-M",
		"q35",
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
		"-audiodev",
		f"wav,id=snd0,path={capture}",
		"-device",
		"ich9-intel-hda,id=hda0",
		"-device",
		"hda-output,bus=hda0.0,audiodev=snd0",
		"-drive",
		f"if=none,id=stick,file={fs_img},format=raw,cache=directsync",
		"-device",
		"usb-ehci,id=ehci",
		"-device",
		"usb-storage,bus=ehci.0,drive=stick",
	]
	master, slave = pty.openpty()
	attrs = termios.tcgetattr(master)
	attrs[3] &= ~(termios.ECHO | termios.ICANON)
	termios.tcsetattr(master, termios.TCSANOW, attrs)
	proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
	os.close(slave)
	return master, proc


def run_one(iso: Path, fs_img: Path, capture: Path, line: str, hold: float) -> None:
	"""Boot, run one play command, wait for audio, then reboot so the WAV closes."""
	master, proc = boot(iso, fs_img, capture)
	try:
		wait_for(master, proc, PROMPT, 45.0)
		send(master, line)
		wait_for(master, proc, "playing", 8.0)
		drain(master, hold)
		send(master, "stop")
		wait_for(master, proc, PROMPT, 5.0)
		send(master, "reboot")
		try:
			proc.wait(timeout=10)
		except subprocess.TimeoutExpired:
			proc.kill()
			proc.wait()
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)


def main() -> int:
	iso = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.iso")
	fs_img = Path("audios-fs.img")
	if not iso.is_file() or not fs_img.is_file():
		print("missing audios.iso or audios-fs.img", file=sys.stderr)
		return 1

	cases = (
		("sine", "tone sine 440 0.5 800ms", 1.1, 440.0),
		("square", "tone square 440 0.5 800ms", 1.1, 440.0),
		("saw", "tone saw 440 0.5 800ms", 1.1, 440.0),
		("noise", "tone noise 440 0.4 800ms", 1.1, 440.0),
		("silence", "tone silence 440 0.5 500ms", 0.8, 440.0),
		("wav", "play audio/test.wav", 0.7, 440.0),
	)
	outdir = Path("audios-sound")
	outdir.mkdir(exist_ok=True)
	for kind, line, hold, hz in cases:
		wav = outdir / f"{kind}.wav"
		print(f"capture {kind}: {line}")
		run_one(iso, fs_img, wav, line, hold)
		if not wav.is_file() or wav.stat().st_size < 256:
			raise RuntimeError(f"{kind}: empty capture {wav}")
		print(" ", judge(kind, hz, str(wav)))
	print("PCM in QEMU matches sine/square/saw/noise/silence/WAV play")
	print("analog SB710/ALC662 jack is not measured here")
	return 0


if __name__ == "__main__":
	try:
		sys.exit(main())
	except Exception as exc:  # noqa: BLE001
		print(exc, file=sys.stderr)
		sys.exit(1)
