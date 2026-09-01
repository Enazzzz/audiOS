# audiOS 0.0.6 — Music system

v0.0.5 is the persistent FAT32 USB. This release is the in-kernel
music system: named audio buffers, WAV I/O, integer DSP, a small
command language, and a non-grid sequencer.

The kernel is freestanding x86_64 with **no FPU/SSE**. Every
operation is integer / fixed-point. A phase vocoder is out of scope.

## Clips

A clip is a named stereo s16 buffer with its own sample rate:

- Up to 8 clips, shared 1 MiB pool (~256k stereo frames total)
- Per-clip cap ~192k frames (fits a WAV in the 768 KiB I/O buffer)
- Sample-accurate access via `sample`
- Metadata via `clip <name>` (rate, frames, duration, peak, RMS-ish peak)

WAV load keeps the file's sample rate (no hidden resample). WAV save
writes PCM s16 stereo. `play` converts clip rate → engine rate on the fly.

## Honest DSP

| Op | What it actually does |
|---|---|
| `pitch` | Linear resample. Duration changes with pitch. `keep` stretches back. |
| `stretch` | Linear interpolate to a new length. Pitch is only approximately held. |
| `rate` | Resample to a new sample rate (duration in seconds stays close). |
| `lpf`/`hpf`/`bpf` | One-pole IIR (Q15). Not a Butterworth. |
| `delay` | Delay line + optional feedback. |
| `rec` | Records the **output mix** (what you hear), not a separate ADC yet. Analog jack-in needs an HDA capture stream that this board's playback path does not use. |

## Timing

`seq add <clip> <time>` takes seconds (`0.5s`), milliseconds (`250ms`),
or frames (`48000` / `48000f`). No grid. `seq render` mixes onto a
clip at sample positions.

## Scripting

`proc <clip> op args…` runs a chain. `script <file>` runs lines from
FAT (comments start with `#`). Line length is 256; up to 24 args.

## Errors

Bad files, missing clips, and impossible sizes print a message and
leave the shell running. No panics.
