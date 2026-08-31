"""Measure whether a captured WAV contains a strong tone near `hz`."""

from __future__ import annotations

import math
import struct
import sys
import wave


def goertzel(samples: list[float], rate: int, freq: float) -> float:
    """Return Goertzel power at `freq` for a real sequence."""
    n = len(samples)
    if n < 32:
        return 0.0
    k = int(0.5 + n * freq / rate)
    w = 2.0 * math.pi * k / n
    coeff = 2.0 * math.cos(w)
    s0 = 0.0
    s1 = 0.0
    s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2 = s1
        s1 = s0
    return s1 * s1 + s2 * s2 - coeff * s1 * s2


def load_mono(path: str) -> tuple[int, list[float]]:
    """Load a WAV and return (rate, mixed mono floats)."""
    with wave.open(path, "r") as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        n = w.getnframes()
        raw = w.readframes(n)
    samples: list[float] = []
    if sw != 2:
        raise RuntimeError(f"expected s16, got width {sw}")
    count = len(raw) // 2
    for i in range(0, count, ch):
        left = struct.unpack_from("<h", raw, i * 2)[0]
        if ch >= 2:
            right = struct.unpack_from("<h", raw, (i + 1) * 2)[0]
            samples.append((left + right) / 2.0 / 32768.0)
        else:
            samples.append(left / 32768.0)
    return rate, samples


def main() -> int:
    path = sys.argv[1]
    expect = float(sys.argv[2] if len(sys.argv) > 2 else 440)
    rate, samples = load_mono(path)
    if len(samples) < rate // 20:
        print(f"capture too short: {len(samples)} frames @ {rate}", file=sys.stderr)
        return 1
    # Skip silence at the start
    peak = max(abs(x) for x in samples)
    if peak < 0.01:
        print(f"capture is silent (peak {peak:.4f})", file=sys.stderr)
        return 1
    p_tone = goertzel(samples, rate, expect)
    p_off = goertzel(samples, rate, expect * 1.5)
    print(f"rate {rate} frames {len(samples)} peak {peak:.3f} tone {p_tone:.1f} off {p_off:.1f}")
    if p_tone < p_off * 2 or p_tone < 1.0:
        print("no dominant tone at", expect, "Hz", file=sys.stderr)
        return 1
    print(f"detected ~{expect:.0f} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
