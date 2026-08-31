# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio, sound design, and experimentation with sound. It is not a Unix
clone and not a DAW.

```
audiOS 0.0.2
96 kHz • 24-bit • 2 channels
audiOS>
```

## What 0.0.2 does

- Boots on x86-64 PCs (BIOS and UEFI) and under QEMU
- Detects PCI audio devices and plays through Intel AC97
- Configures sample rate (44.1 / 48 / 96 kHz), format, channels, buffer
- Generates sine, square, saw, noise, and silence
- Plays PCM WAV files from the ISO
- Reports status, underruns, and recoverable errors without crashing

```
audiOS> tone sine 440Hz 0.5 3s
playing...
```

## Commands

`help` `clear` `version` `cpu` `mem` `reboot`

`audio` `audio devices` `audio info` `audio set` `audio status` `audio test`

`tone sine|square|saw|noise|silence [freq] [amp] [duration]`

`play <file.wav>` `stop`

## Build

Needs a host GCC, GNU Make, `xorriso`, `curl`, `git`, `python3`, and QEMU.

```
make
make test
make run
```

`make run` attaches an AC97 device and writes `audios-out.wav` from QEMU.
On real hardware with ICH AC97, write the ISO to a USB stick or CD.

## Out of scope

GUI, networking, filesystems, effects, synths beyond test tones, MIDI,
Ambisonics, HRTF, and a DAW.
