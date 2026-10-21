#include "proto/profile.h"
#include <string.h>

#define FIELD(name, title, tag_value)                                                              \
    { title, tag_value, offsetof(profile_fields_t, name), sizeof(((profile_fields_t *)0)->name) }
const profile_field_spec_t PROFILE_FIELDS[PROFILE_FIELD_COUNT] = {
    FIELD(display_name, "Display name", TAG_DISPLAY_NAME),
    FIELD(status, "Status", TAG_PROFILE_STATUS),
    FIELD(bio, "Bio", TAG_BIO),
    FIELD(pronouns, "Pronouns", TAG_PRONOUNS),
    FIELD(location, "Location", TAG_LOCATION),
    FIELD(website, "Website", TAG_WEBSITE)};
#undef FIELD

char *profile_field(profile_fields_t *fields, int index) {
    return (char *)fields + PROFILE_FIELDS[index].offset;
}

const char *profile_field_const(const profile_fields_t *fields, int index) {
    return (const char *)fields + PROFILE_FIELDS[index].offset;
}

/* Profiles contain single-line UTF-8 fields. Reject truncation and controls
 * before persisting them or displaying a response from a remote server. */
static bool valid_text(const uint8_t *text, size_t length) {
    for (size_t i = 0; i < length;) {
        uint32_t cp = text[i++];
        if (cp < 0x80) {
            if (cp < 32 || cp == 127)
                return false;
            continue;
        }
        unsigned count;
        uint32_t minimum;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 0x1f;
            count = 1;
            minimum = 0x80;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 0xf;
            count = 2;
            minimum = 0x800;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            count = 3;
            minimum = 0x10000;
        } else
            return false;
        if (i + count > length)
            return false;
        while (count--) {
            if ((text[i] & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (text[i++] & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f))
            return false;
    }
    return true;
}

int profile_fields_read(const uint8_t *payload, uint32_t length, profile_fields_t *fields) {
    memset(fields, 0, sizeof(*fields));
    for (int i = 0; i < PROFILE_FIELD_COUNT; i++) {
        uint16_t size;
        const uint8_t *value = tlv_find(payload, length, PROFILE_FIELDS[i].tag, &size);
        if (!value || size >= PROFILE_FIELDS[i].capacity || !valid_text(value, size))
            return -1;
        memcpy(profile_field(fields, i), value, size);
    }
    return 0;
}

int profile_fields_write(tlv_builder_t *builder, const profile_fields_t *fields) {
    for (int i = 0; i < PROFILE_FIELD_COUNT; i++)
        if (tlv_put_str(builder, PROFILE_FIELDS[i].tag, profile_field_const(fields, i)) < 0)
            return -1;
    return 0;
}
