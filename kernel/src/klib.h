#ifndef AUDIOS_KLIB_H
#define AUDIOS_KLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/** First occurrence of `c` in `s`, or NULL. */
char *strchr(char *s, int c);

/** Last `/` component of a path. */
const char *path_basename(const char *path);

/** True if `s` is `prefix` plus a terminator or extra characters. */
int str_starts(const char *s, const char *prefix);

/** Format a string into `buf`. Supports %s %c %d %u %x %lu %llu %%. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...);

/** `ksnprintf` variant that takes an already-started `va_list`. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
