/* CupidChat public interface. */
#pragma once
#include "proto/types.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FRAME_HEADER_SIZE 12u
#define FRAME_MAX_PAYLOAD (1024u * 1024u)
#define FRAME_MAX_TOTAL (FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD)
#define FRAME_FLAG_PRIORITY_LOW 0x0001u
#define FRAME_FLAG_PRIORITY_HIGH 0x0002u

typedef struct {
    uint32_t length;
    uint16_t type, flags;
    uint32_t seq;
} frame_header_t;

typedef struct {
    frame_header_t hdr;
    uint8_t *payload;
} frame_t;

typedef struct {
    uint8_t *buf;
    size_t cap, len, consumed;
} frame_parser_t;

typedef enum { PARSE_ERROR = -1, PARSE_NEED_MORE = 0, PARSE_FRAME_OK = 1 } parse_result_t;

int frame_parser_init(frame_parser_t *p);
void frame_parser_free(frame_parser_t *p);
int frame_parser_push(frame_parser_t *p, const uint8_t *data, size_t n);
parse_result_t frame_parser_next(frame_parser_t *p, frame_t *out);
ssize_t frame_encode(uint16_t type, uint16_t flags, uint32_t seq, const uint8_t *payload,
                     uint32_t length, uint8_t **out_buf);
void frame_free(frame_t *f);
void frame_dump(const frame_t *f, int fd);
