"""Drive the audiOS shell over QEMU serial and check the 0.1 command set."""

from __future__ import annotations

import os
import pty
import re
import select
import subprocess
import sys
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
    """Type a command followed by CR, matching a real serial user."""
    os.write(master, (line + "\r").encode("ascii"))


def main() -> int:
    iso = Path(sys.argv[1] if len(sys.argv) > 1 else "audios.iso")
    if not iso.is_file():
        print(f"missing ISO: {iso}", file=sys.stderr)
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
    ]
    master, slave = pty.openpty()
    import termios

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
        if "audiOS 0.1" not in text:
            raise RuntimeError(f"banner missing\n{text}")
        if "96 kHz" not in text or "24-bit" not in text:
            raise RuntimeError(f"audio identity line missing\n{text}")

        send(master, "help")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "audiOS commands" not in text:
            raise RuntimeError(f"help failed\n{text}")

        send(master, "version")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "audiOS 0.1" not in text or "uptime" not in text:
            raise RuntimeError(f"version failed\n{text}")

        send(master, "cpu")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "Processor" not in text or "vendor:" not in text:
            raise RuntimeError(f"cpu failed\n{text}")

        send(master, "mem")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "Physical memory" not in text or "usable:" not in text:
            raise RuntimeError(f"mem failed\n{text}")

        send(master, "audio")
        text = wait_for(master, proc, PROMPT, 5.0)
        for fragment in (
            "Audio subsystem",
            "status: initializing",
            "sample rate: 96000 Hz",
            "bit depth: 24-bit",
            "channels: 2",
        ):
            if fragment not in text:
                raise RuntimeError(f"audio missing {fragment!r}\n{text}")

        send(master, "clear")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "audiOS 0.1" not in text:
            raise RuntimeError(f"clear failed to reprint banner\n{text}")

        send(master, "not-a-command")
        text = wait_for(master, proc, PROMPT, 5.0)
        if "no such command" not in text:
            raise RuntimeError(f"unknown command handling failed\n{text}")

        send(master, "reboot")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("reboot did not reset the machine") from exc

        print("audiOS smoke test passed")
        print("checked: help, version, cpu, mem, audio, clear, unknown, reboot")
        return 0
    finally:
        os.close(master)
        if proc.poll() is None:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 — surface the captured serial log
        print(exc, file=sys.stderr)
        sys.exit(1)
