#include "klib.h"

#include <stdarg.h>

/** Copy `n` bytes from `src` to `dest`. Regions must not overlap. */
void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	uint8_t *restrict d = dest;
	const uint8_t *restrict s = src;
	for (size_t i = 0; i < n; i++) {
		d[i] = s[i];
	}
	return dest;
}

/** Fill `n` bytes of `s` with `c`. */
void *memset(void *s, int c, size_t n)
{
	uint8_t *p = s;
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)c;
	}
	return s;
}

/** Copy `n` bytes, correctly when the regions overlap. */
void *memmove(void *dest, const void *src, size_t n)
{
	uint8_t *d = dest;
	const uint8_t *s = src;
	if ((uintptr_t)s > (uintptr_t)d) {
		for (size_t i = 0; i < n; i++) {
			d[i] = s[i];
		}
	} else if ((uintptr_t)s < (uintptr_t)d) {
		for (size_t i = n; i > 0; i--) {
			d[i - 1] = s[i - 1];
		}
	}
	return dest;
}

/** Compare `n` bytes. Returns 0 if equal. */
int memcmp(const void *s1, const void *s2, size_t n)
{
	const uint8_t *a = s1;
	const uint8_t *b = s2;
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return a[i] < b[i] ? -1 : 1;
		}
	}
	return 0;
}

/** Return the length of a NUL-terminated string. */
size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n] != '\0') {
		n++;
	}
	return n;
}

/** Compare two NUL-terminated strings. */
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

/** Compare at most `n` characters of two strings. */
int strncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i] || a[i] == '\0') {
			return (unsigned char)a[i] - (unsigned char)b[i];
		}
	}
	return 0;
}

/** Append one character, stopping at the buffer limit. */
static void buf_putc(char *buf, size_t size, size_t *off, char c)
{
	if (*off + 1 < size) {
		buf[*off] = c;
	}
	(*off)++;
}

/** Write an unsigned number in `base` (10 or 16). */
static void buf_uint(char *buf, size_t size, size_t *off, uint64_t value, unsigned base, int width)
{
	char tmp[32];
	const char *digits = "0123456789abcdef";
	int n = 0;
	do {
		tmp[n++] = digits[value % base];
		value /= base;
	} while (value != 0);
	while (n < width) {
		tmp[n++] = '0';
	}
	while (n > 0) {
		buf_putc(buf, size, off, tmp[--n]);
	}
}

/**
 * Format `fmt` into `buf` of `size` bytes. Always NUL-terminates when size > 0.
 * Returns the number of characters that would have been written (excluding NUL).
 */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	size_t off = 0;
	for (const char *p = fmt; *p != '\0'; p++) {
		if (*p != '%') {
			buf_putc(buf, size, &off, *p);
			continue;
		}
		p++;
		int width = 0;
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (*p - '0');
			p++;
		}
		if (*p == '%') {
			buf_putc(buf, size, &off, '%');
			continue;
		}
		if (*p == 's') {
			const char *s = va_arg(ap, const char *);
			if (s == NULL) {
				s = "(null)";
			}
			while (*s) {
				buf_putc(buf, size, &off, *s++);
			}
			continue;
		}
		if (*p == 'c') {
			buf_putc(buf, size, &off, (char)va_arg(ap, int));
			continue;
		}
		if (*p == 'd') {
			int v = va_arg(ap, int);
			if (v < 0) {
				buf_putc(buf, size, &off, '-');
				buf_uint(buf, size, &off, (uint64_t)(-(int64_t)v), 10, 0);
			} else {
				buf_uint(buf, size, &off, (uint64_t)v, 10, 0);
			}
			continue;
		}
		if (*p == 'u') {
			buf_uint(buf, size, &off, va_arg(ap, unsigned), 10, width);
			continue;
		}
		if (*p == 'x') {
			buf_uint(buf, size, &off, va_arg(ap, unsigned), 16, 0);
			continue;
		}
		if (*p == 'l' && p[1] == 'u') {
			p++;
			buf_uint(buf, size, &off, va_arg(ap, unsigned long), 10, 0);
			continue;
		}
		if (*p == 'l' && p[1] == 'l' && p[2] == 'u') {
			p += 2;
			buf_uint(buf, size, &off, va_arg(ap, unsigned long long), 10, width);
			continue;
		}
		if (*p == 'l' && p[1] == 'x') {
			p++;
			buf_uint(buf, size, &off, va_arg(ap, unsigned long), 16, 0);
			continue;
		}
		buf_putc(buf, size, &off, '%');
		buf_putc(buf, size, &off, *p);
	}
	if (size > 0) {
		size_t term = off < size ? off : size - 1;
		buf[term] = '\0';
	}
	return (int)off;
}

/** Format `fmt` into `buf`, forwarding to `kvsnprintf`. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = kvsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/** First occurrence of `c` in `s`, or NULL. */
char *strchr(char *s, int c)
{
	for (; *s != '\0'; s++) {
		if ((unsigned char)*s == (unsigned char)c) {
			return s;
		}
	}
	return NULL;
}

/** Last path component after the final slash. */
const char *path_basename(const char *path)
{
	const char *slash = path;
	const char *p = path;
	if (path == NULL || path[0] == '\0') {
		return "";
	}
	while (*p != '\0') {
		if (*p == '/' && p[1] != '\0') {
			slash = p + 1;
		}
		p++;
	}
	return slash;
}

/** True when `s` begins with `prefix`. */
int str_starts(const char *s, const char *prefix)
{
	while (*prefix != '\0') {
		if (*s != *prefix) {
			return 0;
		}
		s++;
		prefix++;
	}
	return 1;
}
