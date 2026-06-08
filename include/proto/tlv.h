#ifndef PROTO_TLV_H
#define PROTO_TLV_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include "proto/types.h"

typedef uint16_t tlv_tag_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} tlv_builder_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} tlv_reader_t;

typedef struct {
    uint16_t tag;
    uint16_t len;
    const uint8_t *val;
} tlv_field_t;

int tlv_builder_init(tlv_builder_t *b);
void tlv_builder_free(tlv_builder_t *b);

int tlv_put_bytes(tlv_builder_t *b, tlv_tag_t tag, const uint8_t *val, uint16_t len);
int tlv_put_str(tlv_builder_t *b, tlv_tag_t tag, const char *s);
int tlv_put_u8(tlv_builder_t *b, tlv_tag_t tag, uint8_t v);
int tlv_put_u16(tlv_builder_t *b, tlv_tag_t tag, uint16_t v);
int tlv_put_u32(tlv_builder_t *b, tlv_tag_t tag, uint32_t v);
int tlv_put_u64(tlv_builder_t *b, tlv_tag_t tag, uint64_t v);
uint8_t *tlv_builder_take(tlv_builder_t *b, uint32_t *out_len);

void tlv_reader_init(tlv_reader_t *r, const uint8_t *buf, size_t len);
bool tlv_reader_next(tlv_reader_t *r, tlv_field_t *f);

const uint8_t *tlv_find(const uint8_t *payload, uint32_t plen, tlv_tag_t tag, uint16_t *out_len);
bool tlv_read_str(const uint8_t *payload, uint32_t plen, tlv_tag_t tag, char *dst, size_t dstmax);
bool tlv_read_u8(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint8_t *v);
bool tlv_read_u16(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint16_t *v);
bool tlv_read_u32(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint32_t *v);
bool tlv_read_u64(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint64_t *v);

#endif
