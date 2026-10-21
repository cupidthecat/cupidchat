#include "client/forms.h"
#include "client/text.h"
#include <limits.h>
#include <ncurses.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

int form_char_count(const char *text, int end) {
    int count = 0;
    for (int offset = 0; offset < end && text[offset]; offset = text_next(text, offset))
        count++;
    return count;
}

bool form_edit(char *text, size_t capacity, int *cursor, int key, uint32_t character,
               bool printable) {
    int length = (int)strlen(text);
    if (*cursor > length)
        *cursor = length;
    if (key == KEY_LEFT)
        *cursor = text_prev(text, *cursor);
    else if (key == KEY_RIGHT)
        *cursor = text_next(text, *cursor);
    else if (key == KEY_HOME || key == 1)
        *cursor = 0;
    else if (key == KEY_END || key == 5)
        *cursor = length;
    else if (key == 21) {
        explicit_bzero(text, capacity);
        *cursor = 0;
    } else if (key == 11)
        explicit_bzero(text + *cursor, capacity - (size_t)*cursor);
    else if ((key == KEY_BACKSPACE || key == 127 || key == '\b') && *cursor > 0) {
        int start = text_prev(text, *cursor);
        memmove(text + start, text + *cursor, (size_t)(length - *cursor + 1));
        explicit_bzero(text + length - (*cursor - start) + 1, (size_t)(*cursor - start));
        *cursor = start;
    } else if (key == KEY_DC && *cursor < length) {
        int end = text_next(text, *cursor);
        memmove(text + *cursor, text + end, (size_t)(length - end + 1));
        explicit_bzero(text + length - (end - *cursor) + 1, (size_t)(end - *cursor));
    } else if (printable && character >= 32 && iswprint(character)) {
        mbstate_t state = {0};
        char encoded[MB_LEN_MAX];
        size_t bytes = wcrtomb(encoded, (wchar_t)character, &state);
        if (bytes == (size_t)-1 || (size_t)length + bytes >= capacity)
            return false;
        memmove(text + *cursor + bytes, text + *cursor, (size_t)(length - *cursor + 1));
        memcpy(text + *cursor, encoded, bytes);
        *cursor += (int)bytes;
    } else
        return false;
    return true;
}
