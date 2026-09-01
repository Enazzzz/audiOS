# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio. v0.0.4 is built around one machine: the **ASRock 960GM-GS3 FX**
(AMD FX, 760G/SB710, Realtek ALC662).

```
audiOS 0.0.4
96 kHz • 24-bit • 2 channels
audiOS>
```

## What 0.0.4 does

- Boots on that AM3+ board (legacy BIOS, VGA or GTX 1050, PS/2 keyboard)
- Plays through the onboard HD Audio jack (SB710 + ALC662)
- `tone` with no duration keeps playing until `stop`
- Up / Down arrows recall previous commands (PowerShell-style)
- Console scroll stays in RAM so the picture does not hitch and audio
  does not underrun
- Falls back to Intel ICH AC97 when that is the only controller (QEMU)

```
audiOS> tone sine 440Hz 0.5
playing...
continuous — stop to end
```

Plug speakers or headphones into the **green rear jack**.

## Commands

`help` `clear` `version` `cpu` `mem` `reboot`

`audio` `audio devices` `audio info` `audio set` `audio status` `audio test`

`tone sine|square|saw|noise|silence [freq] [amp] [duration]`

`play <file.wav>` `stop`

Up / Down: previous / next command.

## Build and boot on the FX box

Needs a host GCC, GNU Make, `xorriso`, `curl`, `git`, `python3`, and QEMU.

```
make
make test
```

Flash `audios.iso` with Rufus in **DD image mode** (or `dd`). On the
ASRock: USB in a rear 2.0 port, PS/2 keyboard, VGA monitor, **F11** boot
menu. Do not use the serial port unless the screen stays black.

`make run` uses QEMU's HDA codec and writes `audios-out.wav`.

## Out of scope

GUI, networking, disk install, USB keyboards, effects, MIDI, and a DAW.
USB audio and HDMI audio are not this board's analog path.
