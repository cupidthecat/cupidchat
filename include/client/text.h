#pragma once
#include <stddef.h>

/* Offsets are UTF-8 byte positions; widths are terminal cells. */
int text_next(const char *text, int offset);
int text_prev(const char *text, int offset);
int text_width(const char *text, int begin, int end);
int text_cell_offset(const char *text, int begin, int cells);
int text_wrap_rows(const char *text, int columns);
int text_row_offset(const char *text, int columns, int row);
int text_offset_row(const char *text, int columns, int offset);
void text_wrap_line(const char *text, int columns, int row, char *out, size_t capacity);
int text_expand_emoji(const char *text, char *out, size_t capacity);

typedef struct {
    const char *name;
    const char *glyph;
} emoji_t;

extern const emoji_t EMOJIS[];
extern const int EMOJI_COUNT;
