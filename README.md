# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio. v0.0.5 is built around one machine: the **ASRock 960GM-GS3 FX**
(AMD FX, 760G/SB710, Realtek ALC662).

```
audiOS 0.0.5
96 kHz • 24-bit • 2 channels
audiOS>
```

## What 0.0.5 does

- Boots on that AM3+ board (legacy BIOS, VGA or GTX 1050, PS/2 keyboard)
- Plays through the onboard HD Audio jack (SB710 + ALC662)
- Persistent **FAT32** on the boot USB (EHCI mass storage). Never formats.
- `ls` `cd` `pwd` `mkdir` `rm` `cp` `mv` `cat` `touch` `info` `storage` `mount`
- `play file.wav` from the disk (Limine modules remain a fallback)
- `tone` with no duration keeps playing until `stop`
- Up / Down arrows recall previous commands (PowerShell-style)
- Console scroll stays in RAM so the picture does not hitch and audio
  does not underrun
- Falls back to Intel ICH AC97 when that is the only controller (QEMU)

```
audiOS> ls
  audio/
audiOS> play audio/test.wav
playing...
```

Plug speakers or headphones into the **green rear jack**.

## Commands

`help` `clear` `version` `cpu` `mem` `reboot`

`audio` `audio devices` `audio info` `audio set` `audio status` `audio test`

`tone sine|square|saw|noise|silence [freq] [amp] [duration]`

`play <file.wav>` `stop`

`ls` `cd` `pwd` `mkdir` `rm` `cp` `mv` `cat` `touch` `info` `storage` `mount`

Up / Down: previous / next command.

## Build and boot on the FX box

Needs a host GCC, GNU Make, `xorriso`, `curl`, `git`, `python3`, and QEMU.

```
make
make test
```

For **persistent files**, flash **`audios.img`** with Rufus in **DD image
mode** (or `dd`). That image is MBR + FAT32; the kernel mounts it and
writes stay on the stick across reboot. `audios.iso` still boots but is
read-only — the kernel will not format it.

On the Asrock: USB in a rear 2.0 port, PS/2 keyboard, VGA monitor,
**F11** boot menu.

`make run` uses QEMU's HDA codec plus a USB FAT disk (`audios-fs.img`).

## Out of scope

GUI, networking, disk install, USB keyboards, auto-format, effects, MIDI,
and a DAW. USB audio and HDMI audio are not this board's analog path.
