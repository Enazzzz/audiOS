# audiOS 0.0.5 — Persistent filesystem

The FX board has no HDD. The bootable USB stick **is** the disk. This
release mounts an existing FAT32 volume on that stick (EHCI + USB MSC)
and never formats, partitions, or otherwise destroys the medium.

## Why FAT32

Limine already boots from FAT. Rufus/`dd` of a FAT disk image is how
this machine is flashed. WAV files and user files live on the same
volume as `/boot/kernel`.

ISO9660 (the old `audios.iso`) is read-only. Persistence requires
flashing `audios.img` (MBR + FAT32). The kernel still boots from the
ISO for QEMU; tests attach a second USB FAT disk.

## What the kernel does

1. Find PCI EHCI (class `0C:03` prog-if `0x20`, including AMD SB710).
2. BIOS handoff, port reset, enumerate high-speed devices (root ports
   plus one hub level).
3. Talk SCSI BOT to the first mass-storage LUN.
4. Parse MBR (types `0x0B`/`0x0C`) or GPT (EFI / basic data).
5. Mount only if the BPB is FAT32 with a valid `0x55AA` signature.
6. If anything fails: print an error, leave the shell usable, **do not
   write a new filesystem**.

## User-visible commands

`ls` `cd` `pwd` `mkdir` `rm` `cp` `mv` `cat` `touch` `info` `storage` `mount`

- Absolute and relative paths, `.` / `..`, cwd.
- `rm` deletes a file or an empty directory.
- `mv` renames or moves; `cp` copies file bytes.
- `play file.wav` reads the WAV from the FAT volume (Limine modules
  remain a fallback).
- `info` prints type and size. `storage` prints capacity and free space.

## Out of scope

USB HID, USB audio, auto-format, `mkfs`, journaling, exFAT/NTFS, AHCI.
