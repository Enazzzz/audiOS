#ifndef AUDIOS_CLIP_H
#define AUDIOS_CLIP_H

#include <stdbool.h>
#include <stdint.h>

#define CLIP_NAME	16
#define CLIP_MAX	8

struct clip {
	char name[CLIP_NAME];
	uint32_t rate;
	uint32_t frames;
	int16_t *pcm;	/* interleaved L/R s16, lives in the clip pool */
	int used;
};

/** Steal DMA memory for the clip pool and DSP scratch. */
void clip_init(void);

/** Find a used clip by name, or NULL. */
struct clip *clip_find(const char *name);

/** Load a PCM WAV from FAT or a Limine module into a named clip. */
struct clip *clip_load_file(const char *path, const char *name);

/** Music-system commands (`load`, `gain`, `proc`, `seq`, …). */
void music_cmd(int argc, char **argv);

/** True if `verb` is a music-system command. */
int music_is_verb(const char *verb);

/** Short music help (also printed by `music`). */
void music_help(void);

/** Clip last loaded / `use`d, or NULL. */
struct clip *clip_current(void);
const char *clip_current_name(void);
void clip_use(const char *name);

/** Undo / redo the last clip-mutating op (one snapshot). */
int clip_undo(void);
int clip_redo(void);

/** Session helpers. */
uint32_t seq_get_bpm(void);
void seq_set_bpm(uint32_t bpm);

#endif
