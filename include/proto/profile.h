#pragma once
#include "proto/tlv.h"

#define PROFILE_FIELD_COUNT 6

typedef struct {
    char display_name[65], status[129], bio[513], pronouns[33], location[65], website[161];
} profile_fields_t;

typedef struct {
    const char *label;
    tlv_tag_t tag;
    size_t offset, capacity;
} profile_field_spec_t;

extern const profile_field_spec_t PROFILE_FIELDS[PROFILE_FIELD_COUNT];
char *profile_field(profile_fields_t *fields, int index);
const char *profile_field_const(const profile_fields_t *fields, int index);
bool proto_text_valid(const uint8_t *text, size_t length);
int profile_fields_read(const uint8_t *payload, uint32_t length, profile_fields_t *fields);
int profile_fields_write(tlv_builder_t *builder, const profile_fields_t *fields);

typedef struct {
    profile_fields_t fields;
    char account[MAX_NICK_LEN + 1], nick[MAX_NICK_LEN + 1], away_msg[256];
    uint32_t user_id;
    uint64_t member_since;
    uint8_t presence;
    bool is_account, is_self, is_admin;
} profile_info_t;
