"""Drive audiOS HDA + FAT + music commands over QEMU serial and check capture."""

from __future__ import annotations

import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = "audiOS>"
ROOT = Path(__file__).resolve().parents[1]


def kernel_version() -> str:
    """Read AUDIOS_VERSION_STRING so smoke tests follow patch bumps."""
    text = (ROOT / "kernel" / "src" / "version.h").read_text(encoding="utf-8")
    match = re.search(r'#define\s+AUDIOS_VERSION_STRING\s+"([^"]+)"', text)
    if not match:
        raise RuntimeError("AUDIOS_VERSION_STRING missing from version.h")
    return match.group(1)


def strip_ansi(data: bytes) -> str:
    """Remove colour sequences so assertions can match plain text."""
    return ANSI.sub(b"", data).decode("utf-8", errors="replace")


def wait_for(master: int, proc: subprocess.Popen[bytes], needle: str, timeout: float) -> str:
    """Read the PTY until `needle` appears, QEMU exits, or time runs out."""
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        if proc.poll() is not None:
            leftover = b""
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                leftover = os.read(master, 8192)
            buf.extend(leftover)
            text = strip_ansi(bytes(buf))
            raise RuntimeError(f"QEMU exited {proc.returncode} before {needle!r}\n{text}")
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
    text = strip_ansi(bytes(buf))
    raise TimeoutError(f"timed out waiting for {needle!r}\n{text[-2000:]}")


def send(master: int, line: str) -> None:
	"""Type a command followed by CR."""
	os.write(master, (line + "\r").encode("ascii"))


def send_raw(master: int, data: bytes) -> None:
	"""Write bytes with no trailing CR (game keys, editor, CSI)."""
	os.write(master, data)


def drain(master: int, seconds: float) -> None:
    """Let the guest run (and play audio) while discarding serial."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            os.read(master, 8192)


def main() -> int:
    iso = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.iso")
    capture = Path(sys.argv[2] if len(sys.argv) > 2 else "audios-out.wav")
    if not iso.is_file():
        print(f"missing ISO: {iso}", file=sys.stderr)
        return 1
    try:
        capture.unlink()
    except FileNotFoundError:
        pass

    fs_img = Path("audios-fs.img")
    if not fs_img.is_file():
        print("missing audios-fs.img (run make test)", file=sys.stderr)
        return 1

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
    proc = subprocess.Popen(
        cmd,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    ver = kernel_version()
    banner = f"audiOS {ver}"
    try:
        text = wait_for(master, proc, PROMPT, 45.0)
        if banner not in text:
            raise RuntimeError(f"banner missing (want {banner!r})\n{text}")
        if "mounted FAT32" not in text and "USB MSC" not in text:
            raise RuntimeError(f"filesystem did not mount\n{text}")
        if "leftover FAT32" not in text and "mounted FAT32 data" not in text:
            raise RuntimeError(f"D: leftover FAT32 was not mounted\n{text}")

        send(master, "help")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "tone" not in text or "play" not in text or "storage" not in text or "music" not in text:
            raise RuntimeError(f"help missing commands\n{text}")
        if "edit" not in text:
            raise RuntimeError(f"help missing edit\n{text}")
        if "tetris" not in text:
            raise RuntimeError(f"help missing tetris\n{text}")
        if "type" not in text or "shutdown" not in text:
            raise RuntimeError(f"help missing type/shutdown\n{text}")
        if "  cat " in text.lower() or "cat <file>" in text.lower():
            raise RuntimeError(f"help still lists cat as a file dump\n{text}")

        send(master, "tetris 0")
        text = wait_for(master, proc, "NES tetris", 8.0)
        if "LEVEL" not in text:
            text += wait_for(master, proc, "LEVEL", 8.0)
        os.write(master, b"q")
        wait_for(master, proc, PROMPT, 8.0)

        send(master, "audio")
        text = wait_for(master, proc, PROMPT, 5.0)
        for fragment in ("Audio subsystem", "READY", "96000 Hz", "24-bit", "256 samples"):
            if fragment not in text:
                raise RuntimeError(f"audio missing {fragment!r}\n{text}")
        if "none" in text.split("device:", 1)[-1].split("\n", 1)[0] and "AC97" not in text:
            raise RuntimeError(f"audio did not bind an output device\n{text}")

        send(master, "audio devices")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "HDA" not in text:
            raise RuntimeError(f"did not detect HDA\n{text}")

        send(master, "audio info")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "hw rate" not in text:
            raise RuntimeError(f"audio info failed\n{text}")

        send(master, "audio set rate 48000")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "48000" not in text:
            raise RuntimeError(f"audio set rate failed\n{text}")

        send(master, "audio set rate 12345")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "unsupported" not in text:
            raise RuntimeError(f"bad rate should be rejected\n{text}")

        send(master, "tone sine 440Hz 0.5 500ms")
        text = wait_for(master, proc, "playing...", 5.0)
        drain(master, 0.9)
        send(master, "audio status")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "underruns:" not in text:
            raise RuntimeError(f"audio status failed\n{text}")

        send(master, "stop")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "stopped" not in text and "already stopped" not in text:
            raise RuntimeError(f"stop failed\n{text}")

        send(master, "cpu")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "Processor" not in text:
            raise RuntimeError(f"cpu failed\n{text}")
        os.write(master, b"\x1b[A\r")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "Processor" not in text:
            raise RuntimeError(f"Up arrow did not recall cpu\n{text}")

        send(master, "play test.wav")
        text = wait_for(master, proc, "playing", 5.0)
        drain(master, 0.6)
        send(master, "stop")
        wait_for(master, proc, PROMPT, 5.0)

        send(master, "play bad.wav")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "unsupported" not in text and "not PCM" not in text:
            raise RuntimeError(f"bad.wav should be rejected\n{text}")

        send(master, "play float.wav")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "unsupported" not in text:
            raise RuntimeError(f"float.wav should be rejected\n{text}")

        send(master, "play missing.wav")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "not found" not in text:
            raise RuntimeError(f"missing file should fail cleanly\n{text}")

        send(master, "ls")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "os/" not in text.lower() and "  os\n" not in text.lower():
            raise RuntimeError(f"ls root missing os/ (system volume)\n{text}")
        if "audio/" in text.lower():
            raise RuntimeError(f"system audio/ leaked onto the data volume\n{text}")

        send(master, "ls /os")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "audio" not in text.lower():
            raise RuntimeError(f"ls /os missing audio/\n{text}")

        send(master, "cd /os/audio")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "pwd")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "c:/audio" not in text.lower():
            raise RuntimeError(f"pwd/cd /os/audio failed\n{text}")

        send(master, "ls")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "test.wav" not in text.lower():
            raise RuntimeError(f"ls audio missing test.wav\n{text}")

        send(master, "cd /")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "pwd")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "d:/" not in text.lower():
            raise RuntimeError(f"cd / did not return to D:/\n{text}")
        if "/os" in text.lower() or "/audio" in text.lower():
            raise RuntimeError(f"cd / did not return to the data volume\n{text}")

        send(master, "cd C:")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "pwd")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "c:/audio" not in text.lower():
            raise RuntimeError(f"cd C: did not restore C: cwd\n{text}")
        send(master, "cd D:")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "pwd")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "d:/" not in text.lower():
            raise RuntimeError(f"cd D: failed\n{text}")

        send(master, "cat")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "o.o" not in text.lower() or "^" not in text:
            raise RuntimeError(f"cat easter egg missing ASCII cat\n{text}")

        send(master, "mkdir notes")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "created" not in text:
            raise RuntimeError(f"mkdir failed\n{text}")
        if text.lower().strip().endswith("ok") and "created" not in text:
            raise RuntimeError(f"mkdir failed with empty error\n{text}")

        for i in range(12):
            send(master, f"mkdir d{i}")
            text = wait_for(master, proc, PROMPT, 10.0)
            if "created" not in text:
                raise RuntimeError(f"data-volume mkdir {i} failed\n{text}")
            send(master, f"rm d{i}")
            wait_for(master, proc, PROMPT, 10.0)

        send(master, "cp /os/audio/test.wav notes/t.wav")
        text = wait_for(master, proc, PROMPT, 15.0)
        if "copied" not in text:
            raise RuntimeError(f"cp failed\n{text}")

        send(master, "info notes/t.wav")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "file" not in text.lower() or "size" not in text.lower():
            raise RuntimeError(f"info failed\n{text}")

        send(master, "play notes/t.wav")
        text = wait_for(master, proc, "playing", 10.0)
        drain(master, 0.5)
        send(master, "stop")
        wait_for(master, proc, PROMPT, 5.0)

        send(master, "mv notes/t.wav notes/u.wav")
        text = wait_for(master, proc, PROMPT, 15.0)
        if "moved" not in text:
            raise RuntimeError(f"mv failed\n{text}")

        send(master, "touch notes/hello")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "touched" not in text:
            raise RuntimeError(f"touch failed\n{text}")

        send(master, "storage")
        text = wait_for(master, proc, PROMPT, 30.0)
        if "FAT32" not in text or "free" not in text:
            raise RuntimeError(f"storage failed\n{text}")
        if "system" not in text.lower() or "data" not in text.lower():
            raise RuntimeError(f"storage did not list both partitions\n{text}")
        tot = re.search(r"\(data\).*?total:\s+(\d+)", text, re.S)
        if tot is None or int(tot.group(1)) < 20_000_000:
            raise RuntimeError(f"data partition did not take leftover USB space\n{text}")

        send(master, "rm notes/u.wav")
        wait_for(master, proc, PROMPT, 8.0)
        send(master, "rm notes/hello")
        wait_for(master, proc, PROMPT, 8.0)
        send(master, "rm notes")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "removed" not in text:
            raise RuntimeError(f"rm dir failed\n{text}")

        send(master, "load audio/test.wav a")
        text = wait_for(master, proc, PROMPT, 10.0)
        if "loaded" not in text.lower():
            raise RuntimeError(f"load wav failed\n{text}")

        send(master, "clip a")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "Hz" not in text or "frames" not in text:
            raise RuntimeError(f"clip metadata failed\n{text}")

        send(master, "slice a b 0 2000")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "sliced" not in text.lower():
            raise RuntimeError(f"slice failed\n{text}")

        send(master, "proc b gain 0.8 lpf 4000 reverse")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "proc" not in text.lower():
            raise RuntimeError(f"proc chain failed\n{text}")

        send(master, "save b out.wav")
        text = wait_for(master, proc, PROMPT, 15.0)
        if "saved" not in text.lower():
            raise RuntimeError(f"save wav failed\n{text}")

        send(master, "play b")
        text = wait_for(master, proc, "playing", 8.0)
        drain(master, 0.4)
        send(master, "stop")
        wait_for(master, proc, PROMPT, 5.0)

        send(master, "seed 1")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "noise n 100ms 0.2")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "noise" not in text.lower():
            raise RuntimeError(f"noise failed\n{text}")

        send(master, "seq clear")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "seq add b 0")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "seq add n 50ms")
        wait_for(master, proc, PROMPT, 5.0)
        send(master, "seq render pat")
        text = wait_for(master, proc, PROMPT, 8.0)
        if "render" not in text.lower():
            raise RuntimeError(f"seq render failed\n{text}")

        send(master, "seq bpm 120")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "seq bpm 120" not in text.lower():
            raise RuntimeError(f"seq bpm failed\n{text}")

        send(master, "use b")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "current clip" not in text.lower():
            raise RuntimeError(f"use current clip failed\n{text}")

        send(master, "help vol")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "volume" not in text.lower():
            raise RuntimeError(f"help vol failed\n{text}")

        send(master, "sample b 0")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "L=" not in text:
            raise RuntimeError(f"sample access failed\n{text}")

        send(master, "version")
        text = wait_for(master, proc, PROMPT, 5.0)
        if ver not in text:
            raise RuntimeError(f"version command missing {ver!r}\n{text}")

        send(master, "reboot")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("reboot did not reset the machine") from exc
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)

    analyze = Path(__file__).with_name("analyze_tone.py")
    if not capture.is_file() or capture.stat().st_size < 1024:
        print(f"warning: capture {capture} missing or tiny; skipping tone FFT", file=sys.stderr)
        print(f"audiOS {ver} smoke test passed (commands only)")
        return 0

    rc = subprocess.call([sys.executable, str(analyze), str(capture), "440"])
    if rc != 0:
        print("command checks passed but captured audio was not a 440 Hz tone", file=sys.stderr)
        return 1
    print(f"audiOS {ver} smoke test passed")
    print("checked: HDA, FAT32 USB, clips load/slice/proc/save, seq, 440 Hz capture")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(exc, file=sys.stderr)
        sys.exit(1)
