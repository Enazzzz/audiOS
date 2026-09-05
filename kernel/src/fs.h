#ifndef AUDIOS_FS_H
#define AUDIOS_FS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Probe USB MSC, mount the system FAT32, and if leftover space is free,
 * create a separate data partition. Never reformats the boot volume.
 */
void fs_init(void (*idle)(void));

bool fs_ready(void);
const char *fs_error(void);

void fs_cmd_ls(int argc, char **argv);
void fs_cmd_cd(int argc, char **argv);
void fs_cmd_pwd(void);
void fs_cmd_mkdir(int argc, char **argv);
void fs_cmd_rm(int argc, char **argv);
void fs_cmd_cp(int argc, char **argv);
void fs_cmd_mv(int argc, char **argv);
void fs_cmd_type(int argc, char **argv);
void fs_cmd_touch(int argc, char **argv);
void fs_cmd_info(int argc, char **argv);
void fs_cmd_storage(void);
void fs_cmd_mount(void);
void fs_cmd_drives(void);
void fs_cmd_update(int argc, char **argv);

/** True if `token` is `C:` / `D:` / `E:` (optional slash). */
int fs_is_drive(const char *token);

/** Read a whole file from the mounted volume. */
bool fs_read_file(const char *path, void *buf, uint32_t cap, uint32_t *out_size);

/** Write a whole file (creates or replaces). Never formats the volume. */
bool fs_write_file(const char *path, const void *buf, uint32_t size);

/** Shared I/O buffer used by WAV load/save. */
uint8_t *fs_iobuf(uint32_t *cap);

#endif
