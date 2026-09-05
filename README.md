# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio. **v0.2.0** is built around one machine: the **ASRock 960GM-GS3 FX**
(AMD FX, 760G/SB710, Realtek ALC662).

```
audiOS 0.2.0
96 kHz • 24-bit • 2 channels
1024x768 framebuffer • 128x48 text
audiOS>
```

Versions are `MAJOR.MINOR.PATCH` in `kernel/src/version.h`. Every small
change bumps **patch** (`0.1.0` → `0.1.1` → …). A distinct capability jump
bumps **minor**. A huge turning point becomes **1.0.0** (v1.00). The old
`0.0.2`–`0.0.6` trail and the first-kernel `0.1` tag are history; this
line started as a naming reset at `0.1.0`, not a rollback.

## What 0.2.0 does

- Boots on that AM3+ board (legacy BIOS, VGA or GTX 1050). **PS/2 keyboard
  only.** `.img` boot takes EHCI for the stick, which kills BIOS USB-legacy,
  so a USB keyboard (including a PS/2-to-USB dongle) will not type. Plug a
  real PS/2 keyboard into the PS/2 port.
- Plays through the onboard HD Audio jack (SB710 + ALC662)
- Persistent **FAT32** on the boot USB (EHCI mass storage). **C:** is the
  64 MiB system volume (Limine, kernel, stock `audio/`). **D:** is leftover
  space on the same stick. If that leftover **already looks like FAT32**
  (an old D: after a 64 MiB image flash wiped the MBR slot), 0.2.0 **mounts
  it and writes type 0x0C back** — it does not skip it and it does not
  format it. Fresh leftover is still mkfs'd once. **E:** is a second USB
  if `mount` finds one. `/os` is C:. Keep lasting files on D:.
- USB MSC recovers after a failed sector (BOT reset) so storage does not
  stay dead until reboot. I/O is still one 512-byte sector at a time.
- **Updates without wiping D:** do not raw-flash a new `audios.img` over
  the whole stick. `update`, `update-system.ps1`, or `update_system.py`
  write C: only. First install is still a full image flash.
- `help` is alphabetical. `help <command>` prints parameters. `cat` is
  only an ASCII cat (`type` dumps files).
- Line editor: shift-select highlight, Ctrl-C copy, Ctrl-V paste,
  Ctrl-Backspace (or Ctrl-W) delete word. PgUp depth is 400 lines.
- `tetris [level]` is NES-rules falling blocks with **2×2 cells**. The OS
  owns the paint frame (quiet serial, skip unchanged cells). Hold
  left/right is NES DAS (16 then every 6). Scores: `D:/tetris.scr`, else C:.
- Live meters top-right: volume, limiter flag, line-out / mic / line-in
  bars. **F11 / F12** volume, **F5** play/pause, **F6** stop.
- `audio vol`, `audio limiter on|off` (on for headphones), `audio gain`
  for analog input. `rec mic` / `rec line` on the ALC662 jacks (QEMU's
  hda-output has no ADC).
- Current clip: `use name`, then `proc gain 0.5`, `gain 0.5`, `play`.
  `slice` previews. `undo` / `redo`. `mark a|b` / `cue` for accurate cuts.
  `seq bpm` / `seq play 3` / `seq play bar 2`.
- Session: `D:/session.aos` (magic `AOS1`). Boot asks y/n for 2 s (default
  n). `D:/autoexec.aos` or `C:/autoexec.aos` runs if it starts with `AOS1`.
- `shutdown` tries QEMU ports and ACPI S5 on PM1a **and** PM1b. If the
  board stays on, control returns to the shell — do not yank the cord.
- Limine is asked for `1920x1080x32`; it picks the closest mode the GPU
  or VBIOS actually has (onboard VGA is often 1024×768). The kernel sizes
  the text grid from that framebuffer.

```
audiOS> load /os/audio/test.wav a
audiOS> use a
audiOS> proc gain 0.8 lpf 4000 delay 180 0.35 0.2
audiOS> save a wet.wav
audiOS> play
```

Plug speakers or headphones into the **green rear jack**. Headphones: 
`audio limiter on`. Speakers can leave it off.

## Commands

`help` lists every command alphabetically. `help <command>` is the
parameter page. Highlights:

`audio vol|limiter|gain|pause` `tone` `play` `stop` `music`

`load` `save` `use` `clip` `slice` `mark` `cue` `undo` `redo` `proc`
`seq bpm|play` `rec` `rec mic` `rec line`

`ls` `cd` `pwd` `mkdir` `rm` `cp` `mv` `type` `touch` `info` `storage`
`mount` `drives` `update` `edit` `tetris` `script` `reboot` `shutdown`

`cd C:` `cd D:` `cd E:` — system / data / extra USB. `/os` is C:.

Keys: Up/Down history, PgUp/PgDn scroll, F5/F6 transport, F11/F12 volume.

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

For **persistent files**, first install is a full raw **`audios.img`**:
`.\tools\flash-latest-release.ps1 -Elevate`, Balena Etcher, Rufus **DD
image mode**, or `dd`. Etcher is fine; a Limine **Stage 3 file not
found** panic was a FAT geometry bug in the image (too few clusters, so
Limine treated the volume as FAT16), not the flasher.

That image is MBR + two FAT32 partitions after first boot: **C:** system
(the 64 MiB image) and **D:** data. Later kernel bumps — **do not
full-flash again** if you want to keep D:. On Windows:

```
.\tools\update-system.ps1 -Elevate
```

That downloads the latest release and writes C: only. `flash-latest-release.ps1`
now refuses a stick that already has D: unless you pass `-Full`. Linux:
`python3 tools/update_system.py /dev/sdX audios.img`. On the board:
`update`. Do not flash `audios.iso` on this machine — AMI legacy BIOS
treats USB as a hard disk, and Limine then cannot see an ISO9660 stage 3.

On the Asrock: USB in a rear 2.0 port, PS/2 keyboard, VGA monitor,
**F11** boot menu. IGP VGA is typically 1024×768 from VBIOS; a GTX 1050
can take the 1920×1080 request.

`make run` uses QEMU's HDA codec plus a USB FAT disk (`audios-fs.img`).

## Out of scope

GUI, networking, disk install, USB keyboards, auto-format of an existing
user partition, MIDI, a DAW GUI, and float DSP (phase vocoder). USB audio
and HDMI audio are not this board's analog path.
