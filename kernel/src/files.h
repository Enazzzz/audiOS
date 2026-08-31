#ifndef AUDIOS_FILES_H
#define AUDIOS_FILES_H

#include <limine.h>
#include <stddef.h>
#include <stdint.h>

struct audio_file {
	const char *name;
	const uint8_t *data;
	size_t size;
};

/** Index Limine modules so `play` can look them up by basename. */
void files_init(struct limine_module_response *modules);

/** Find a loaded file by basename (e.g. test.wav). */
const struct audio_file *files_find(const char *name);

/** Number of loaded named files. */
unsigned files_count(void);

/** File at `index`, or NULL. */
const struct audio_file *files_at(unsigned index);

#endif
