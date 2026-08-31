#ifndef AUDIOS_FONT_H
#define AUDIOS_FONT_H

#include <stdint.h>

#define FONT_WIDTH	8
#define FONT_HEIGHT	16
#define FONT_GLYPHS	128
#define FONT_BULLET	7

/** 8x16 glyphs for U+0000..U+007F. Bit 0 of each row is the leftmost pixel. */
extern const uint8_t font8x16[FONT_GLYPHS][FONT_HEIGHT];

#endif
