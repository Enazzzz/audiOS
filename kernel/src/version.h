#ifndef AUDIOS_VERSION_H
#define AUDIOS_VERSION_H

/*
 * Persistent system identity. These values are compiled into the kernel
 * image so version information survives across shell sessions and reboots
 * of the same build.
 *
 * Scheme (MAJOR.MINOR.PATCH):
 *   PATCH  — every small change (0.1.0 → 0.1.1 → 0.1.2 …)
 *   MINOR  — a distinct capability jump that is not yet “the OS”
 *   MAJOR  — a huge turning point. 1.0.0 (spoken as v1.00) is the goal.
 *
 * History: 0.1 was the first Limine kernel, then 0.0.2–0.0.6 grew audio,
 * FAT, and music. 0.1.0 reset naming. Keyboard input is PS/2 (8042);
 * USB HID was dropped because it did not work on the FX board.
 * 0.1.8 adds a text-mode falling-block game using NES Tetris rules.
 * 0.1.9 is the FX-board report: C:/ D:/ E: drives, in-place update,
 * editor wrap, shutdown, tetris scores, quieter console.
 * 0.2.0 mounts leftover D: FAT32, OS-managed Tetris frames, 2×2 cells,
 * live meters, session restore, and .aos scripts.
 * 0.3.0 is the second machine: 1.44 MB Limine floppy (format + chainloader),
 * PageUp that actually pages, HUD overlay that does not eat the console.
 * 0.3.1: floppy format/install no longer die with "irq timeout" (poll-off,
 * no-op seek, MSR ACTA fallback when IRQ6 is not routed).
 * 0.3.2: FX board still timed out with msr=0x81 (ACTA stuck until Sense
 * Interrupt). Wait then always SIS; try unit 1; Linux 1.44 Specify.
 * 0.3.3: format/install name write-protect (ST3 wp / ST1 NW) instead of
 * a generic fail — Epson SMD-300 reports the 3.5\" tab hole.
 * 0.3.4: ST3 wp is the drive pin, not the tab. Do not abort format on it
 * (Sense Drive Status can stay set with the hole covered). Spin the motor
 * 500 ms before sensing so the SMD-300 WP LED is on. Fail only on ST1 NW.
 * 3.5" HD has two holes: left = density (always open), right = WP tab.
 * 0.4.0: Audio Link — first analog modem (Manchester 2400, superseded).
 * 0.5.0: Pulse PHY (48 kHz stereo 12-bit PAM, ~1 Mbit/s, 1 ms frames) and
 * a real 32-bit slave OS for the ASUS A7V333. FX is master; A7V333 is slave.
 *
 * Bump these three macros and AUDIOS_VERSION_STRING / AUDIOS_BANNER
 * together. Tests read AUDIOS_VERSION_STRING from this file.
 */
#define AUDIOS_NAME		"audiOS"
#define AUDIOS_VERSION_MAJOR	0
#define AUDIOS_VERSION_MINOR	5
#define AUDIOS_VERSION_PATCH	0
#define AUDIOS_VERSION_STRING	"0.5.0"
#define AUDIOS_BANNER		"audiOS 0.5.0"
#define AUDIOS_AUDIO_RATE	96000u
#define AUDIOS_AUDIO_BITS	24u
#define AUDIOS_AUDIO_CHANNELS	2u
#define AUDIOS_BOARD		"ASRock 960GM-GS3 FX"
#define AUDIOS_SLAVE_BOARD	"ASUS A7V333"
#define AUDIOS_SLAVE_BANNER	"audiOS slave 0.5.0"

#endif
