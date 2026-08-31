# audiOS

audiOS is a lightweight, command-line-first operating system for digital
audio, sound design, and experimentation with sound. It is not a Unix
clone and not a DAW. The first milestone boots a 64-bit kernel and drops
you into a dedicated musical-computer shell.

```
audiOS 0.1
96 kHz • 24-bit • 2 channels
audiOS>
```

## What 0.1 does

- Boots on x86-64 PCs (BIOS and UEFI) and under QEMU
- 64-bit kernel with interrupt handling and a 1000 Hz system timer
- CPU identification and physical memory map
- Keyboard and framebuffer console, mirrored to serial
- Persistent version identity compiled into the kernel
- `audio` reports the subsystem even though the engine is not loaded yet

## Commands

`help` `clear` `version` `cpu` `mem` `audio` `reboot`

## Build

Needs a host GCC, GNU Make, `xorriso`, `curl`, and `git`. QEMU is required
for `make test` / `make run`.

```
make
make test
make run
```

`make` produces `audios.iso`. `make run` boots it in QEMU with the serial
console attached to your terminal. On real hardware, write the ISO to a
USB stick or CD; an AMD FX-class BIOS machine is a supported target.

## Architecture

Limine loads the ELF64 kernel. The kernel owns the PIC, IDT, PIT, PS/2
keyboard, COM1, and a dark framebuffer console. Audio is a kernel object
with reserved stream interfaces so later DSP, routing, and synthesis work
does not have to be bolted on as an application.

## Out of scope for 0.1

GUI, networking, filesystems, user accounts, effects, synthesizers,
Ambisonics, HRTF, and a package ecosystem.
