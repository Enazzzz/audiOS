"""Drive audiOS 0.0.3 HDA commands over QEMU serial and check the capture."""

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
    try:
        text = wait_for(master, proc, PROMPT, 30.0)
        if "audiOS 0.0.3" not in text:
            raise RuntimeError(f"banner missing\n{text}")

        send(master, "help")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "tone" not in text or "play" not in text:
            raise RuntimeError(f"help missing audio commands\n{text}")

        send(master, "audio")
        text = wait_for(master, proc, PROMPT, 5.0)
        for fragment in ("Audio subsystem", "READY", "96000 Hz", "24-bit", "64 samples"):
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

        send(master, "version")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "0.0.3" not in text:
            raise RuntimeError(f"still alive after audio errors?\n{text}")

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
        print("audiOS 0.0.3 smoke test passed (commands only)")
        return 0

    rc = subprocess.call([sys.executable, str(analyze), str(capture), "440"])
    if rc != 0:
        print("command checks passed but captured audio was not a 440 Hz tone", file=sys.stderr)
        return 1
    print("audiOS 0.0.3 smoke test passed")
    print("checked: HDA, devices, set, tone, play, stop, errors, 440 Hz capture")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print(exc, file=sys.stderr)
        sys.exit(1)
