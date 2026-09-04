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
 *
 * Bump these three macros and AUDIOS_VERSION_STRING / AUDIOS_BANNER
 * together. Tests read AUDIOS_VERSION_STRING from this file.
 */
#define AUDIOS_NAME		"audiOS"
#define AUDIOS_VERSION_MAJOR	0
#define AUDIOS_VERSION_MINOR	1
#define AUDIOS_VERSION_PATCH	2
#define AUDIOS_VERSION_STRING	"0.1.2"
#define AUDIOS_BANNER		"audiOS 0.1.2"
#define AUDIOS_AUDIO_RATE	96000u
#define AUDIOS_AUDIO_BITS	24u
#define AUDIOS_AUDIO_CHANNELS	2u
#define AUDIOS_BOARD		"ASRock 960GM-GS3 FX"

#endif
