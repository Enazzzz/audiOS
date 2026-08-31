"""Write the ISO test WAV files (PCM 16-bit stereo 440 Hz)."""

from __future__ import annotations

import math
import struct
import sys
import wave
from pathlib import Path


def write_pcm16_stereo(path: Path, hz: int, seconds: float, amp: float = 0.5, rate: int = 48000) -> None:
    """Write a sine tone as little-endian PCM WAV."""
    n = int(rate * seconds)
    with wave.open(str(path), "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            s = int(amp * 32767 * math.sin(2 * math.pi * hz * i / rate))
            frames += struct.pack("<hh", s, s)
        w.writeframes(frames)


def write_float_wav(path: Path) -> None:
    """Write an IEEE-float WAV that the kernel must reject."""
    data = b"RIFF" + struct.pack("<I", 36) + b"WAVE"
    fmt = struct.pack("<HHIIHH", 3, 1, 48000, 48000 * 4, 4, 32)
    data += b"fmt " + struct.pack("<I", 16) + fmt
    data += b"data" + struct.pack("<I", 0)
    # Fix RIFF size
    data = b"RIFF" + struct.pack("<I", len(data) - 8) + data[8:]
    path.write_bytes(data)


def main() -> None:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "media")
    out.mkdir(parents=True, exist_ok=True)
    write_pcm16_stereo(out / "test.wav", 440, 0.4, 0.5, 48000)
    (out / "bad.wav").write_bytes(b"NOT-A-WAV-FILE")
    write_float_wav(out / "float.wav")
    print("wrote", out / "test.wav")


if __name__ == "__main__":
    main()
