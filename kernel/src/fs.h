#ifndef AUDIOS_FS_H
#define AUDIOS_FS_H

#include <stdbool.h>
#include <stdint.h>

/** Probe USB MSC, mount FAT32 if present, never format. */
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
void fs_cmd_cat(int argc, char **argv);
void fs_cmd_touch(int argc, char **argv);
void fs_cmd_info(int argc, char **argv);
void fs_cmd_storage(void);
void fs_cmd_mount(void);

/** Read a whole file from the mounted volume. */
bool fs_read_file(const char *path, void *buf, uint32_t cap, uint32_t *out_size);

#endif
