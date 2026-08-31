#include "files.h"
#include "klib.h"

#define FILES_MAX	8

static struct audio_file files[FILES_MAX];
static unsigned file_count;

/** Index Limine modules so `play` can look them up by basename. */
void files_init(struct limine_module_response *modules)
{
	file_count = 0;
	if (modules == NULL) {
		return;
	}
	for (uint64_t i = 0; i < modules->module_count && file_count < FILES_MAX; i++) {
		struct limine_file *m = modules->modules[i];
		if (m == NULL || m->address == NULL) {
			continue;
		}
		const char *path = m->path ? m->path : m->string;
		if (path == NULL) {
			path = "module";
		}
		files[file_count].name = path_basename(path);
		files[file_count].data = (const uint8_t *)m->address;
		files[file_count].size = (size_t)m->size;
		file_count++;
	}
}

/** Find a loaded file by basename. */
const struct audio_file *files_find(const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	const char *want = path_basename(name);
	for (unsigned i = 0; i < file_count; i++) {
		if (strcmp(files[i].name, want) == 0) {
			return &files[i];
		}
	}
	return NULL;
}

/** Number of loaded named files. */
unsigned files_count(void)
{
	return file_count;
}

/** File at `index`, or NULL. */
const struct audio_file *files_at(unsigned index)
{
	if (index >= file_count) {
		return NULL;
	}
	return &files[index];
}
