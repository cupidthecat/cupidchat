#ifndef PROTO_FRAME_H
#define PROTO_FRAME_H

#include <stdint.h>
#include <sys/types.h>

#define FRAME_HEADER_SIZE 12
#define FRAME_MAX_PAYLOAD 65535
#define FRAME_MAX_TOTAL (FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD)

#define FRAME_FLAG_NONE           0x00
#define FRAME_FLAG_PRIORITY_HIGH  0x01

typedef struct {
    uint32_t length;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
} frame_hdr_t;

typedef struct {
    frame_hdr_t hdr;
    uint8_t *payload;
} frame_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t consumed;
} frame_parser_t;

typedef enum {
    PARSE_FRAME_OK = 0,
    PARSE_NEED_MORE = 1,
    PARSE_ERROR = 2
} parse_result_t;

int frame_parser_init(frame_parser_t *p);
void frame_parser_free(frame_parser_t *p);
int frame_parser_push(frame_parser_t *p, const uint8_t *data, size_t n);
parse_result_t frame_parser_next(frame_parser_t *p, frame_t *out);

ssize_t frame_encode(uint16_t type, uint16_t flags, uint32_t seq,
                     const uint8_t *payload, uint32_t length,
                     uint8_t **out_buf);
void frame_free(frame_t *f);
void frame_dump(const frame_t *f, int fd);

#endif
