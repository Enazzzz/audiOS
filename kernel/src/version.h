#ifndef AUDIOS_VERSION_H
#define AUDIOS_VERSION_H

/*
 * Persistent system identity. These values are compiled into the kernel
 * image so version information survives across shell sessions and reboots
 * of the same build.
 */
#define AUDIOS_NAME		"audiOS"
#define AUDIOS_VERSION_MAJOR	0
#define AUDIOS_VERSION_MINOR	0
#define AUDIOS_VERSION_PATCH	2
#define AUDIOS_VERSION_STRING	"0.0.2"
#define AUDIOS_BANNER		"audiOS 0.0.2"
#define AUDIOS_AUDIO_RATE	96000u
#define AUDIOS_AUDIO_BITS	24u
#define AUDIOS_AUDIO_CHANNELS	2u

#endif
