# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio. **v0.1.8** is built around one machine: the **ASRock 960GM-GS3 FX**
(AMD FX, 760G/SB710, Realtek ALC662).

```
audiOS 0.1.8
96 kHz • 24-bit • 2 channels
audiOS>
```

Versions are `MAJOR.MINOR.PATCH` in `kernel/src/version.h`. Every small
change bumps **patch** (`0.1.0` → `0.1.1` → …). A distinct capability jump
bumps **minor**. A huge turning point becomes **1.0.0** (v1.00). The old
`0.0.2`–`0.0.6` trail and the first-kernel `0.1` tag are history; this
line started as a naming reset at `0.1.0`, not a rollback.

## What 0.1.8 does

- Boots on that AM3+ board (legacy BIOS, VGA or GTX 1050). **PS/2 keyboard
  only.** `.img` boot takes EHCI for the stick, which kills BIOS USB-legacy,
  so a USB keyboard (including a PS/2-to-USB dongle) will not type. Plug a
  real PS/2 keyboard into the PS/2 port.
- Plays through the onboard HD Audio jack (SB710 + ALC662)
- Persistent **FAT32** on the boot USB (EHCI mass storage). Partition 1 is
  the 64 MiB system volume (Limine, kernel, stock `audio/`). Leftover space
  on the stick becomes a second FAT32 **data** volume on first mount. The
  shell starts on the data volume; system files are under `/os`. USB I/O
  is one 512-byte sector at a time so 4 KiB data-volume clusters work on
  the SB710 (creates outside `/os` were failing with `ok`). The boot
  partition is not reformatted. A 0.0.6 stick cannot be split in place —
  flash a new `audios.img`.
- `edit <file>` is a small text editor (`^O` save, `^X` quit). PgUp / PgDn
  scroll the console.
- `tetris [level]` is a text-mode falling-block game using **NES Tetris
  rules** (Nintendo Rotation System, no wall kick, no lock delay, no hard
  drop, NES gravity / DAS / scoring). It is original kernel code, not a
  port of vitetris or other clones. Tetris is a trademark of Tetris
  Holding; this is a fan recreation of the 1989 NES mechanics. PS/2
  arrows hold DAS; Q quits, P pauses, X/Up rotate, Z the other way.
- Named **clips** in RAM: load/save WAV, slice, join, mix, reverse, gain,
  filters, delay, pitch/stretch (integer resample), a non-grid sequencer
- `play clip` or `play file.wav` (`loop` until `stop`)
- `rec` captures the **output mix** (what you hear) so you can process and
  save without extra steps. Analog jack-in is not a separate ADC path yet.
- `tone` with no duration keeps playing until `stop`
- Up / Down arrows recall previous commands (PowerShell-style)
- Console scroll stays in RAM so the picture does not hitch and audio
  does not underrun
- Falls back to Intel ICH AC97 when that is the only controller (QEMU)

```
audiOS> load /os/audio/test.wav a
audiOS> proc a gain 0.8 lpf 4000 delay 180 0.35 0.2
audiOS> save a wet.wav
audiOS> play a
```

Plug speakers or headphones into the **green rear jack**.

## Commands

`help` `clear` `version` `cpu` `mem` `reboot`

`audio` `audio devices` `audio info` `audio set` `audio status` `audio test`

`tone sine|square|saw|noise|silence [freq] [amp] [duration]`

`play <clip|file.wav> [loop|n]` `stop`

`music` — full clip/DSP list. Highlights:

`load` `save` `clips` `clip` `new` `sample` `slice` `join` `mix` `repeat`
`reverse` `gain` `norm` `fade` `pitch` `stretch` `rate` `crush` `decimate`
`distort` `noise` `seed` `lpf` `hpf` `bpf` `delay` `pan` `vary` `proc`
`seq` `rec` `drop` `script`

`ls` `cd` `pwd` `mkdir` `rm` `cp` `mv` `cat` `touch` `info` `storage` `mount`
`edit` `tetris`

Up / Down: previous / next command. PgUp / PgDn: scroll the console.

`pitch` resamples (length follows pitch) unless you add `keep`.
`stretch` changes length. DSP is integer/fixed-point (no FPU on this kernel).

## Images (no local build)

`audios.img` and `audios.iso` are **not** in the git tree. They are 64 MiB
generated disks (like a compiler’s `.o` files) and would bloat history
every time the kernel changed. CI builds them and attaches them to a
GitHub Release:

**https://github.com/Enazzzz/audiOS/releases/latest** — last green `main` build

Every push also creates a numbered release (`build-N`) with the same
files, so a branch image is downloadable before it is merged. CI
artifacts stay on the Actions run for 90 days as well.

## Build and boot on the FX box

Needs a host GCC, GNU Make, `xorriso`, `curl`, `git`, `python3`, and QEMU.

```
make
make test
```

`make test` includes a PCM check (`qemu_sound.py`): sine is a 440 Hz
fundamental, square has odd harmonics, saw has a harmonic series, noise
is not a tone, silence is quiet, `play test.wav` stays 440 Hz, L=R, no
clipping. That is QEMU's HDA capture, not the ASRock analog jack.

For **persistent files**, flash **`audios.img`** as a raw disk: Balena
Etcher, Rufus **DD image mode**, or `dd`. Etcher is fine; a Limine
**Stage 3 file not found** panic was a FAT geometry bug in the image
(too few clusters, so Limine treated the volume as FAT16), not the
flasher. Rebuild `audios.img` and write it again.

That image is MBR + two FAT32 partitions after first boot: system (the
64 MiB image) and data (whatever was left on the stick). `mkdir` and
`save` land on the data volume; `/os` is the system tree. Do not flash
`audios.iso` on this board — AMI legacy BIOS treats USB as a hard disk,
and Limine then cannot see an ISO9660 stage 3.

On the Asrock: USB in a rear 2.0 port, PS/2 keyboard, VGA monitor,
**F11** boot menu.

`make run` uses QEMU's HDA codec plus a USB FAT disk (`audios-fs.img`).

## Out of scope

GUI, networking, disk install, USB keyboards, auto-format of an existing
user partition, MIDI, a DAW GUI, and float DSP (phase vocoder). USB audio
and HDMI audio are not this board's analog path.
