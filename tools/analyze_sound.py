#!/usr/bin/env python3
"""Judge whether a captured WAV matches a tone family (not analog jack timbre)."""

from __future__ import annotations

import math
import struct
import sys
import wave


def goertzel(samples: list[float], rate: int, freq: float) -> float:
	"""Return Goertzel power at `freq` for a real sequence."""
	n = len(samples)
	if n < 32 or freq <= 0 or freq >= rate / 2:
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


def load_stereo(path: str) -> tuple[int, list[float], list[float]]:
	"""Load s16 WAV as (rate, left, right) floats in -1..1."""
	with wave.open(path, "r") as w:
		rate = w.getframerate()
		ch = w.getnchannels()
		sw = w.getsampwidth()
		n = w.getnframes()
		raw = w.readframes(n)
	if sw != 2:
		raise RuntimeError(f"expected s16, got width {sw}")
	left: list[float] = []
	right: list[float] = []
	count = len(raw) // 2
	if ch == 1:
		for i in range(count):
			s = struct.unpack_from("<h", raw, i * 2)[0] / 32768.0
			left.append(s)
			right.append(s)
		return rate, left, right
	for i in range(0, count, ch):
		left.append(struct.unpack_from("<h", raw, i * 2)[0] / 32768.0)
		right.append(struct.unpack_from("<h", raw, (i + 1) * 2)[0] / 32768.0)
	return rate, left, right


def trim_active(samples: list[float], thresh: float = 0.02) -> list[float]:
	"""Keep the loud middle of a capture, skipping HDA startup silence."""
	idx = [i for i, x in enumerate(samples) if abs(x) >= thresh]
	if len(idx) < 64:
		return samples
	a = idx[0]
	b = idx[-1] + 1
	span = b - a
	# Drop attack/decay edges so Goertzel sees a stable period.
	lo = a + span // 8
	hi = b - span // 8
	if hi - lo < 64:
		return samples[a:b]
	return samples[lo:hi]


def rms(samples: list[float]) -> float:
	"""Root-mean-square amplitude."""
	if not samples:
		return 0.0
	return math.sqrt(sum(x * x for x in samples) / len(samples))


def stereo_err(left: list[float], right: list[float]) -> float:
	"""Mean absolute L-R error on overlapping frames."""
	n = min(len(left), len(right))
	if n == 0:
		return 1.0
	return sum(abs(left[i] - right[i]) for i in range(n)) / n


def judge(kind: str, hz: float, path: str) -> str:
	"""
	Return a one-line report or raise RuntimeError if the capture is wrong.

	This is PCM-in-QEMU, not the ALC662 jack. Sine uses a 256-entry LUT so
	harmonics are allowed as long as the fundamental still wins.
	"""
	rate, left, right = load_stereo(path)
	mono = [(left[i] + right[i]) * 0.5 for i in range(min(len(left), len(right)))]
	peak = max((abs(x) for x in mono), default=0.0)
	clip = sum(1 for x in mono if abs(x) >= 0.999)
	kind = kind.lower()

	if kind == "silence":
		if peak > 0.04:
			raise RuntimeError(f"silence not silent: peak {peak:.3f}")
		return f"silence peak {peak:.4f} (ok)"

	body = trim_active(mono)
	if len(body) < rate // 40:
		raise RuntimeError(f"{kind}: capture too short ({len(body)} frames @ {rate})")
	if peak < 0.05:
		raise RuntimeError(f"{kind}: too quiet (peak {peak:.4f})")
	if clip > len(mono) * 0.02:
		raise RuntimeError(f"{kind}: clipped ({clip} samples)")

	lr = stereo_err(left, right)
	# Oscillator is dual-mono. WAV playback is also stereo-identical in test.wav.
	if kind != "noise" and lr > 0.08:
		raise RuntimeError(f"{kind}: L/R mismatch {lr:.3f}")

	p1 = goertzel(body, rate, hz)
	p15 = goertzel(body, rate, hz * 1.5)
	p2 = goertzel(body, rate, hz * 2)
	p3 = goertzel(body, rate, hz * 3)
	p4 = goertzel(body, rate, hz * 4)
	p5 = goertzel(body, rate, hz * 5)
	level = rms(body)

	if kind in ("sine", "wav"):
		if p1 < p15 * 3 or p1 < p2 * 2 or p1 < 1.0:
			raise RuntimeError(
				f"{kind}: fundamental {hz:.0f} Hz not dominant "
				f"(p1={p1:.1f} p1.5={p15:.1f} p2={p2:.1f})"
			)
	elif kind == "square":
		if p1 < p15 * 2 or p3 < p2:
			raise RuntimeError(
				f"square: want odd harmonics (p1={p1:.1f} p2={p2:.1f} p3={p3:.1f})"
			)
	elif kind == "saw":
		if p1 < p15 * 2 or p2 < p15 or p3 < p15:
			raise RuntimeError(
				f"saw: want a harmonic series (p1={p1:.1f} p2={p2:.1f} p3={p3:.1f})"
			)
	elif kind == "noise":
		# Broadband: no single bin should dwarf a nearby off-frequency by a huge ratio.
		if p1 > p15 * 8 and p1 > p2 * 8:
			raise RuntimeError(f"noise looks tonal (p1={p1:.1f} p1.5={p15:.1f})")
		if level < 0.02:
			raise RuntimeError(f"noise too quiet rms={level:.4f}")
	else:
		raise RuntimeError(f"unknown kind {kind!r}")

	return (
		f"{kind} {hz:.0f}Hz peak={peak:.3f} rms={level:.3f} "
		f"LRerr={lr:.4f} p1={p1:.0f} p2={p2:.0f} p3={p3:.0f} p4={p4:.0f} p5={p5:.0f}"
	)


def main() -> int:
	if len(sys.argv) < 3:
		print("usage: analyze_sound.py <wav> sine|square|saw|noise|silence|wav [hz]", file=sys.stderr)
		return 2
	path = sys.argv[1]
	kind = sys.argv[2]
	hz = float(sys.argv[3] if len(sys.argv) > 3 else 440)
	print(judge(kind, hz, path))
	return 0


if __name__ == "__main__":
	try:
		sys.exit(main())
	except Exception as exc:  # noqa: BLE001
		print(exc, file=sys.stderr)
		sys.exit(1)
