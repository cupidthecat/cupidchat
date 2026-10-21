#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UTF-8 editing shared by account forms and profile fields. */
bool form_edit(char *text, size_t capacity, int *cursor, int key, uint32_t character,
               bool printable);
int form_char_count(const char *text, int end);
