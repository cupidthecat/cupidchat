#include "client/text.h"
#include <stdint.h>
#include <string.h>
#include <wchar.h>

const emoji_t EMOJIS[] = {
    {"smile", "😄"},    {"grin", "😀"},       {"joy", "😂"},    {"wink", "😉"}, {"heart", "💜"},
    {"thumbsup", "👍"}, {"thumbsdown", "👎"}, {"wave", "👋"},   {"cry", "😢"},  {"thinking", "🤔"},
    {"fire", "🔥"},     {"party", "🎉"},      {"rocket", "🚀"}, {"eyes", "👀"}, {"coffee", "☕"},
    {"100", "💯"},      {"cool", "😎"},       {"clap", "👏"},   {"pray", "🙏"}, {"cat", "🐱"}};
const int EMOJI_COUNT = (int)(sizeof(EMOJIS) / sizeof(EMOJIS[0]));

static int decode(const char *text, uint32_t *codepoint) {
    const unsigned char *p = (const unsigned char *)text;
    uint32_t value = p[0];
    int length;
    if (value < 0x80) {
        *codepoint = value;
        return value ? 1 : 0;
    }
    if (value >= 0xc2 && value <= 0xdf) {
        value &= 0x1f;
        length = 2;
    } else if (value >= 0xe0 && value <= 0xef) {
        value &= 0x0f;
        length = 3;
    } else if (value >= 0xf0 && value <= 0xf4) {
        value &= 7;
        length = 4;
    } else {
        *codepoint = 0xfffd;
        return 1;
    }
    for (int i = 1; i < length; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            *codepoint = 0xfffd;
            return 1;
        }
        value = (value << 6) | (p[i] & 0x3f);
    }
    if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
        (length == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        *codepoint = 0xfffd;
        return 1;
    }
    *codepoint = value;
    return length;
}

static int cell_width(uint32_t codepoint) {
    int width = wcwidth((wchar_t)codepoint);
    return width < 0 ? 1 : width;
}

int text_next(const char *text, int offset) {
    uint32_t cp;
    return offset + decode(text + offset, &cp);
}

int text_prev(const char *text, int offset) {
    if (offset <= 0)
        return 0;
    offset--;
    while (offset > 0 && ((unsigned char)text[offset] & 0xc0) == 0x80)
        offset--;
    return offset;
}

int text_width(const char *text, int begin, int end) {
    int cells = 0;
    while (begin < end && text[begin]) {
        uint32_t cp;
        int length = decode(text + begin, &cp);
        if (begin + length > end)
            break;
        cells += cell_width(cp);
        begin += length;
    }
    return cells;
}

int text_cell_offset(const char *text, int begin, int cells) {
    int used = 0;
    while (text[begin]) {
        uint32_t cp;
        int length = decode(text + begin, &cp), width = cell_width(cp);
        if (used + width > cells)
            break;
        used += width;
        begin += length;
    }
    return begin;
}

/* Use the same traversal for rendering, scrolling, and resize anchoring. */
static int walk_rows(const char *text, int columns, int target_row, int target_offset) {
    int row = 0, used = 0, offset = 0;
    if (columns < 1)
        columns = 1;
    if (target_row == 0)
        return 0;
    while (text[offset]) {
        uint32_t cp;
        int length = decode(text + offset, &cp), width = cell_width(cp);
        if (used && used + width > columns) {
            row++;
            used = 0;
            if (row == target_row)
                return offset;
        }
        if (target_offset >= 0 && offset >= target_offset)
            return row;
        used += width;
        offset += length;
    }
    if (target_offset >= 0)
        return row;
    return target_row >= 0 ? offset : row + 1;
}

int text_wrap_rows(const char *text, int columns) {
    return walk_rows(text, columns, -1, -1);
}

int text_row_offset(const char *text, int columns, int row) {
    return walk_rows(text, columns, row, -1);
}

int text_offset_row(const char *text, int columns, int offset) {
    return walk_rows(text, columns, -1, offset);
}

void text_wrap_line(const char *text, int columns, int row, char *out, size_t capacity) {
    if (!capacity)
        return;
    int start = text_row_offset(text, columns, row);
    int end = text_row_offset(text, columns, row + 1);
    size_t written = 0;
    while (start < end) {
        uint32_t cp;
        int length = decode(text + start, &cp);
        int invalid = cp == 0xfffd || wcwidth((wchar_t)cp) < 0;
        size_t count = invalid ? 1 : (size_t)length;
        if (written + count >= capacity)
            break;
        if (invalid)
            out[written] = cp < 32 || cp == 127 ? ' ' : '?';
        else
            memcpy(out + written, text + start, count);
        written += count;
        start += length;
    }
    out[written] = '\0';
}

int text_expand_emoji(const char *text, char *out, size_t capacity) {
    size_t written = 0;
    while (*text) {
        const char *piece = text;
        size_t consumed = 1, length = 1;
        if (*text == ':') {
            for (int i = 0; i < EMOJI_COUNT; i++) {
                size_t name_length = strlen(EMOJIS[i].name);
                if (!strncmp(text + 1, EMOJIS[i].name, name_length) &&
                    text[name_length + 1] == ':') {
                    piece = EMOJIS[i].glyph;
                    length = strlen(piece);
                    consumed = name_length + 2;
                    break;
                }
            }
        }
        if (written + length >= capacity)
            return -1;
        memcpy(out + written, piece, length);
        written += length;
        text += consumed;
    }
    if (!capacity)
        return -1;
    out[written] = '\0';
    return 0;
}
