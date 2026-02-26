/*
 * src/shared/proto/frame.c -  frame encode/decode and incremental parser
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>

#include "proto/frame.h"

/* Internal helpers */

static int buf_reserve(frame_parser_t *p, size_t needed) {
    if (p->cap - p->len >= needed) return 0;
    size_t newcap = p->cap ? p->cap * 2 : 4096;
    while (newcap - p->len < needed) newcap *= 2;
    if (newcap > FRAME_MAX_TOTAL * 2) return -1;
    uint8_t *nb = realloc(p->buf, newcap);
    if (!nb) return -1;
    p->buf = nb;
    p->cap = newcap;
    return 0;
}

/* Compact the consumed prefix out of the buffer. */
static void buf_compact(frame_parser_t *p) {
    if (p->consumed == 0) return;
    memmove(p->buf, p->buf + p->consumed, p->len - p->consumed);
    p->len -= p->consumed;
    p->consumed = 0;
}

/* Public API */

int frame_parser_init(frame_parser_t *p) {
    memset(p, 0, sizeof(*p));
    p->buf = malloc(4096);
    if (!p->buf) return -1;
    p->cap = 4096;
    return 0;
}

void frame_parser_free(frame_parser_t *p) {
    free(p->buf);
    p->buf = NULL;
    p->cap = p->len = p->consumed = 0;
}

int frame_parser_push(frame_parser_t *p, const uint8_t *data, size_t n) {
    if (n == 0) return 0;
    buf_compact(p);
    if (buf_reserve(p, n) < 0) return -1;
    memcpy(p->buf + p->len, data, n);
    p->len += n;
    return 0;
}

parse_result_t frame_parser_next(frame_parser_t *p, frame_t *out) {
    buf_compact(p);

    /* need at least a full header */
    if (p->len < FRAME_HEADER_SIZE) return PARSE_NEED_MORE;

    const uint8_t *hdr = p->buf;

    uint32_t length;
    uint16_t type, flags;
    uint32_t seq;
    memcpy(&length, hdr + 0, 4); length = ntohl(length);
    memcpy(&type,   hdr + 4, 2); type   = ntohs(type);
    memcpy(&flags,  hdr + 6, 2); flags  = ntohs(flags);
    memcpy(&seq,    hdr + 8, 4); seq    = ntohl(seq);

    /* validate payload size */
    if (length > FRAME_MAX_PAYLOAD) return PARSE_ERROR;

    size_t total = FRAME_HEADER_SIZE + length;
    if (p->len < total) return PARSE_NEED_MORE;

    /* build frame */
    out->hdr.length = length;
    out->hdr.type   = type;
    out->hdr.flags  = flags;
    out->hdr.seq    = seq;
    out->payload    = NULL;

    if (length > 0) {
        out->payload = malloc(length);
        if (!out->payload) return PARSE_ERROR;
        memcpy(out->payload, p->buf + FRAME_HEADER_SIZE, length);
    }

    p->consumed += total;
    return PARSE_FRAME_OK;
}

ssize_t frame_encode(uint16_t type, uint16_t flags, uint32_t seq,
                     const uint8_t *payload, uint32_t length,
                     uint8_t **out_buf) {
    size_t total = FRAME_HEADER_SIZE + length;
    uint8_t *buf = malloc(total);
    if (!buf) return -1;

    uint32_t nl = htonl(length);
    uint16_t nt = htons(type);
    uint16_t nf = htons(flags);
    uint32_t ns = htonl(seq);

    memcpy(buf + 0, &nl, 4);
    memcpy(buf + 4, &nt, 2);
    memcpy(buf + 6, &nf, 2);
    memcpy(buf + 8, &ns, 4);
    if (length > 0 && payload) memcpy(buf + FRAME_HEADER_SIZE, payload, length);

    *out_buf = buf;
    return (ssize_t)total;
}

void frame_free(frame_t *f) {
    if (f) {
        free(f->payload);
        f->payload = NULL;
    }
}

void frame_dump(const frame_t *f, int fd) {
    char line[256];
    int n = snprintf(line, sizeof(line),
        "[FRAME] type=0x%04x flags=0x%04x seq=%u len=%u\n",
        f->hdr.type, f->hdr.flags, f->hdr.seq, f->hdr.length);
    if (write(fd, line, (size_t)n) != (ssize_t)n) (void)0;

    /* hex dump first 64 bytes of payload */
    uint32_t dump_len = f->hdr.length < 64 ? f->hdr.length : 64;
    for (uint32_t i = 0; i < dump_len; i += 16) {
        n = snprintf(line, sizeof(line), "  %04x:", i);
        for (uint32_t j = i; j < i + 16 && j < dump_len; j++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n,
                          " %02x", f->payload[j]);
        }
        line[n++] = '\n';
        if (write(fd, line, (size_t)n) != (ssize_t)n) (void)0;
    }
}
