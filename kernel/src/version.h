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
 * FAT, music, and HID. 0.1.0 is a naming reset of that current snapshot,
 * not a rollback. Old design docs keep their original titles.
 *
 * Bump these three macros and AUDIOS_VERSION_STRING / AUDIOS_BANNER
 * together. Tests read AUDIOS_VERSION_STRING from this file.
 */
#define AUDIOS_NAME		"audiOS"
#define AUDIOS_VERSION_MAJOR	0
#define AUDIOS_VERSION_MINOR	1
#define AUDIOS_VERSION_PATCH	0
#define AUDIOS_VERSION_STRING	"0.1.0"
#define AUDIOS_BANNER		"audiOS 0.1.0"
#define AUDIOS_AUDIO_RATE	96000u
#define AUDIOS_AUDIO_BITS	24u
#define AUDIOS_AUDIO_CHANNELS	2u
#define AUDIOS_BOARD		"ASRock 960GM-GS3 FX"

#endif
