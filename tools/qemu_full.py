#!/usr/bin/env python3
"""Drive every audiOS shell command over QEMU serial and require a live reply."""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qemu_smoke import PROMPT, drain, kernel_version, send, send_raw, wait_for  # noqa: E402


def expect(master: int, proc, cmd: str, needles: tuple[str, ...], timeout: float = 10.0) -> str:
	"""Send `cmd` and require every needle in the reply before the next prompt."""
	send(master, cmd)
	text = wait_for(master, proc, PROMPT, timeout)
	low = text.lower()
	for needle in needles:
		if needle.lower() not in low:
			raise RuntimeError(f"{cmd!r}: missing {needle!r}\n{text[-2500:]}")
	return text


def boot_iso(iso: Path, fs_img: Path, capture: Path):
	"""Start QEMU with HDA capture and a USB FAT stick (same layout as `make test`)."""
	import pty
	import subprocess
	import termios

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


def main() -> int:
	iso = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.iso")
	fs_img = Path("audios-fs.img")
	capture = Path("audios-full-out.wav")
	if not iso.is_file() or not fs_img.is_file():
		print("missing audios.iso or audios-fs.img (run make test)", file=sys.stderr)
		return 1

	ver = kernel_version()
	master, proc = boot_iso(iso, fs_img, capture)
	checked: list[str] = []
	try:
		text = wait_for(master, proc, PROMPT, 45.0)
		if f"audiOS {ver}" not in text:
			raise RuntimeError(f"banner missing {ver}\n{text}")
		if "mounted FAT32" not in text and "USB MSC" not in text:
			raise RuntimeError(f"FAT did not mount\n{text}")
		checked.append("boot banner + FAT mount")

		expect(master, proc, "help", ("tone", "play", "music", "storage", "reboot", "script", "edit", "tetris"))
		expect(master, proc, "clear", (f"audiOS {ver}", "96 kHz"))
		expect(master, proc, "version", (ver, "build 0.", "ASRock", "uptime"))
		expect(master, proc, "cpu", ("Processor", "vendor:", "family:"))
		expect(master, proc, "mem", ("Physical memory", "usable:"))
		expect(master, proc, "notacommand", ("no such command",))
		checked.append("help/clear/version/cpu/mem/unknown")

		expect(master, proc, "audio", ("Audio subsystem", "READY", "96000 Hz"))
		expect(master, proc, "audio devices", ("HDA",))
		expect(master, proc, "audio info", ("hw rate",))
		expect(master, proc, "audio status", ("underruns:",))
		expect(master, proc, "audio set buffer 128", ("128",))
		expect(master, proc, "audio set format 16", ("16-bit",))
		expect(master, proc, "audio set channels 1", ("channels: 1",))
		expect(master, proc, "audio set channels 2", ("channels: 2",))
		expect(master, proc, "audio set format 24", ("24-bit",))
		expect(master, proc, "audio set buffer 256", ("256",))
		expect(master, proc, "audio set rate 44100", ("44100",))
		expect(master, proc, "audio set rate 96000", ("96000",))
		expect(master, proc, "audio set device 0", ("device:",))
		expect(master, proc, "audio set rate 12345", ("unsupported",))
		send(master, "audio test")
		wait_for(master, proc, "playing...", 5.0)
		drain(master, 0.25)
		expect(master, proc, "stop", ("stopped",))
		checked.append("audio set/status/test")

		for kind in ("sine", "square", "saw", "noise", "silence"):
			send(master, f"tone {kind} 440 0.4 80ms")
			wait_for(master, proc, "playing...", 5.0)
			drain(master, 0.2)
		send(master, "tone sine 880 0.3")
		wait_for(master, proc, "playing...", 5.0)
		drain(master, 0.2)
		expect(master, proc, "stop", ("stopped",))
		expect(master, proc, "tone bogon 440", ("unknown signal",))
		checked.append("tone waveforms + continuous + stop")

		expect(master, proc, "ls", ("os",))
		expect(master, proc, "ls /os", ("audio",))
		expect(master, proc, "cd /os/audio", tuple())
		expect(master, proc, "pwd", ("/os/audio",))
		expect(master, proc, "ls", ("test.wav",))
		expect(master, proc, "cd /", tuple())
		expect(master, proc, "pwd", ("/",))
		expect(master, proc, "edit", ("usage: edit",))
		expect(master, proc, "mkdir scratch", ("created",))
		expect(master, proc, "cp /os/audio/test.wav scratch/t.wav", ("copied",), 20.0)
		expect(master, proc, "info scratch/t.wav", ("file", "size"))
		expect(master, proc, "mv scratch/t.wav scratch/u.wav", ("moved",), 20.0)
		expect(master, proc, "touch scratch/note", ("touched",))
		expect(master, proc, "cat /os/demo.aos", ("pwd", "version"))
		expect(master, proc, "storage", ("FAT32", "free", "data"))
		expect(master, proc, "mount", ("FAT32", "mounted"))
		expect(master, proc, "script /os/demo.aos", ("/", ver), 15.0)
		send(master, "play scratch/u.wav")
		wait_for(master, proc, "playing", 10.0)
		drain(master, 0.3)
		send(master, "stop")
		wait_for(master, proc, PROMPT, 5.0)
		expect(master, proc, "rm scratch/u.wav", ("removed",))
		expect(master, proc, "rm scratch/note", ("removed",))
		expect(master, proc, "rm scratch", ("removed",))
		checked.append("FAT ls/cd/pwd/mkdir/cp/mv/cat/touch/info/storage/mount/script/rm")

		expect(master, proc, "music", ("clips are named", "seq add", "rec "))
		expect(master, proc, "load audio/test.wav a", ("loaded",), 15.0)
		expect(master, proc, "clips", ("a",))
		expect(master, proc, "clip a", ("Hz", "frames"))
		expect(master, proc, "slice a s 0 1500", ("sliced",))
		expect(master, proc, "drop a", ("dropped",))
		expect(master, proc, "new z 40ms", ("new z",))
		expect(master, proc, "sample z 0 1000 2000", ("L=1000", "R=2000"))
		expect(master, proc, "join j s z", ("joined",))
		expect(master, proc, "mix m s z", ("mixed",))
		expect(master, proc, "repeat s r 2", ("repeat",))
		checked.append("load/slice/new/sample/join/mix/repeat")

		ops = (
			("reverse s", ("reverse",)),
			("gain s 0.7", ("gain",)),
			("norm s", ("norm",)),
			("fade s in 10ms", ("fade",)),
			("pitch s 1.2", ("pitch",)),
			("stretch s 0.8", ("stretch",)),
			("rate s 48000", ("rate",)),
			("crush s 8", ("crush",)),
			("decimate s 8000", ("decimate",)),
			("distort s 2", ("distort",)),
			("lpf s 4000", ("lpf",)),
			("hpf s 200", ("hpf",)),
			("bpf s 1000", ("bpf",)),
			("delay s 20 0.4 0.1", ("delay",)),
			("pan s -30", ("pan",)),
			("vary s 0.05", ("vary",)),
		)
		for cmd, needles in ops:
			expect(master, proc, cmd, needles, 8.0)
		expect(master, proc, "proc s gain 0.9 reverse", ("proc",), 8.0)
		expect(master, proc, "save s out.wav", ("saved",), 20.0)
		send(master, "play s")
		wait_for(master, proc, "playing", 8.0)
		drain(master, 0.2)
		send(master, "stop")
		wait_for(master, proc, PROMPT, 5.0)
		send(master, "play s loop")
		wait_for(master, proc, "playing", 8.0)
		drain(master, 0.2)
		expect(master, proc, "stop", ("stopped",))
		expect(master, proc, "drop z", ("dropped",))
		expect(master, proc, "drop j", ("dropped",))
		expect(master, proc, "drop m", ("dropped",))
		expect(master, proc, "drop r", ("dropped",))
		checked.append("DSP verbs + proc/save/play/loop")

		expect(master, proc, "seed 42", ("seed 42",))
		expect(master, proc, "noise n 40ms 0.2", ("noise",))
		expect(master, proc, "seq clear", ("cleared",))
		expect(master, proc, "seq add s 0", ("seq add",))
		expect(master, proc, "seq add n 20ms", ("seq add",))
		expect(master, proc, "seq list", ("seq",))
		expect(master, proc, "seq len 80ms", ("seq len",))
		expect(master, proc, "seq render pat", ("render",), 10.0)
		send(master, "seq play")
		wait_for(master, proc, "playing seq", 10.0)
		drain(master, 0.25)
		send(master, "stop")
		wait_for(master, proc, PROMPT, 5.0)
		expect(master, proc, "rec mix 60ms", ("recording mix",))
		drain(master, 0.25)
		expect(master, proc, "clip mix", ("frames",))
		expect(master, proc, "drop n", ("dropped",))
		expect(master, proc, "drop pat", ("dropped",))
		expect(master, proc, "drop mix", ("dropped",))
		expect(master, proc, "drop s", ("dropped",))
		checked.append("seq/rec/drop/seed/noise")

		send(master, "cpu")
		wait_for(master, proc, PROMPT, 5.0)
		os.write(master, b"\x1b[A\r")
		text = wait_for(master, proc, PROMPT, 5.0)
		if "Processor" not in text:
			raise RuntimeError(f"Up arrow did not recall cpu\n{text}")
		os.write(master, b"\x1b[B")
		drain(master, 0.1)
		checked.append("history Up")

		send(master, "tetris 0")
		text = wait_for(master, proc, "NES tetris", 8.0)
		if "LEVEL" not in text:
			text += wait_for(master, proc, "LEVEL", 8.0)
		send_raw(master, b"q")
		wait_for(master, proc, PROMPT, 8.0)
		checked.append("tetris start/quit")

		send(master, "reboot")
		try:
			proc.wait(timeout=10)
		except Exception as exc:
			raise RuntimeError("reboot did not reset QEMU") from exc
		checked.append("reboot")
	finally:
		if proc.poll() is None:
			proc.kill()
			proc.wait()
		os.close(master)

	print(f"audiOS {ver} full command test passed")
	for item in checked:
		print(f"  ok  {item}")
	print("not covered here: analog SB710/ALC662 jack (needs the FX board), PS/2 (see qemu_ps2.py)")
	return 0


if __name__ == "__main__":
	try:
		sys.exit(main())
	except Exception as exc:  # noqa: BLE001
		print(exc, file=sys.stderr)
		sys.exit(1)
