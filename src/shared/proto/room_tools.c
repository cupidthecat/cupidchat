#include "proto/room_tools.h"
#include "proto/profile.h"
#include <string.h>

bool room_name_valid(const char *name) {
    size_t length = strlen(name);
    if (!length || length > MAX_ROOM_LEN)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

bool room_read_text(const uint8_t *p, uint32_t length, tlv_tag_t tag, char *out, size_t capacity) {
    uint16_t size = 0;
    const uint8_t *raw = tlv_find(p, length, tag, &size);
    if (!raw || size >= capacity || !proto_text_valid(raw, size))
        return false;
    memset(out, 0, capacity);
    memcpy(out, raw, size);
    return true;
}

bool room_read_name(const uint8_t *p, uint32_t length, char out[65]) {
    return room_read_text(p, length, TAG_ROOM, out, 65) && room_name_valid(out);
}
