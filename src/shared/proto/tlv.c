/*
 * src/shared/proto/tlv.c -  binary TLV encoder/decoder
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "proto/tlv.h"

/* Builder */

int tlv_builder_init(tlv_builder_t *b) {
    memset(b, 0, sizeof(*b));
    b->buf = malloc(256);
    if (!b->buf) return -1;
    b->cap = 256;
    return 0;
}

void tlv_builder_free(tlv_builder_t *b) {
    free(b->buf);
    b->buf = NULL;
    b->cap = b->len = 0;
}

static int builder_reserve(tlv_builder_t *b, size_t extra) {
    if (b->cap - b->len >= extra) return 0;
    size_t nc = b->cap * 2;
    while (nc - b->len < extra) nc *= 2;
    uint8_t *nb = realloc(b->buf, nc);
    if (!nb) return -1;
    b->buf = nb;
    b->cap = nc;
    return 0;
}

int tlv_put_bytes(tlv_builder_t *b, tlv_tag_t tag,
                  const uint8_t *val, uint16_t len) {
    size_t need = 4u + len;  /* 2 tag + 2 len + data */
    if (builder_reserve(b, need) < 0) return -1;
    uint16_t nt = htons((uint16_t)tag);
    uint16_t nl = htons(len);
    memcpy(b->buf + b->len + 0, &nt, 2);
    memcpy(b->buf + b->len + 2, &nl, 2);
    if (len > 0 && val) memcpy(b->buf + b->len + 4, val, len);
    b->len += need;
    return 0;
}

int tlv_put_str(tlv_builder_t *b, tlv_tag_t tag, const char *s) {
    size_t slen = s ? strlen(s) : 0;
    if (slen > 65535) slen = 65535;
    return tlv_put_bytes(b, tag, (const uint8_t *)s, (uint16_t)slen);
}

int tlv_put_u8(tlv_builder_t *b, tlv_tag_t tag, uint8_t v) {
    return tlv_put_bytes(b, tag, &v, 1);
}

int tlv_put_u16(tlv_builder_t *b, tlv_tag_t tag, uint16_t v) {
    uint16_t nv = htons(v);
    return tlv_put_bytes(b, tag, (const uint8_t *)&nv, 2);
}

int tlv_put_u32(tlv_builder_t *b, tlv_tag_t tag, uint32_t v) {
    uint32_t nv = htonl(v);
    return tlv_put_bytes(b, tag, (const uint8_t *)&nv, 4);
}

int tlv_put_u64(tlv_builder_t *b, tlv_tag_t tag, uint64_t v) {
    /* big-endian 64-bit */
    uint8_t buf[8];
    for (int i = 7; i >= 0; i--) { buf[i] = (uint8_t)(v & 0xFF); v >>= 8; }
    return tlv_put_bytes(b, tag, buf, 8);
}

uint8_t *tlv_builder_take(tlv_builder_t *b, uint32_t *out_len) {
    uint8_t *p = b->buf;
    *out_len   = (uint32_t)b->len;
    b->buf = NULL;
    b->cap = b->len = 0;
    return p;
}

/* Reader */

void tlv_reader_init(tlv_reader_t *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

bool tlv_reader_next(tlv_reader_t *r, tlv_field_t *f) {
    if (r->pos + 4 > r->len) return false;
    uint16_t tag, len;
    memcpy(&tag, r->buf + r->pos + 0, 2); tag = ntohs(tag);
    memcpy(&len, r->buf + r->pos + 2, 2); len = ntohs(len);
    if (r->pos + 4u + len > r->len) return false;
    f->tag = tag;
    f->len = len;
    f->val = r->buf + r->pos + 4;
    r->pos += 4u + len;
    return true;
}

/* Search helpers */

const uint8_t *tlv_find(const uint8_t *payload, uint32_t plen,
                        tlv_tag_t tag, uint16_t *out_len) {
    tlv_reader_t r;
    tlv_field_t  f;
    tlv_reader_init(&r, payload, plen);
    while (tlv_reader_next(&r, &f)) {
        if (f.tag == (uint16_t)tag) {
            if (out_len) *out_len = f.len;
            return f.val;
        }
    }
    return NULL;
}

bool tlv_read_str(const uint8_t *payload, uint32_t plen,
                  tlv_tag_t tag, char *dst, size_t dstmax) {
    uint16_t vlen;
    const uint8_t *v = tlv_find(payload, plen, tag, &vlen);
    if (!v || dstmax == 0) return false;
    size_t copy = vlen < dstmax ? vlen : dstmax - 1;
    memcpy(dst, v, copy);
    dst[copy] = '\0';
    return true;
}

bool tlv_read_u8(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint8_t *v) {
    uint16_t vlen;
    const uint8_t *r = tlv_find(p, pl, tag, &vlen);
    if (!r || vlen < 1) return false;
    *v = r[0];
    return true;
}

bool tlv_read_u16(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint16_t *v) {
    uint16_t vlen;
    const uint8_t *r = tlv_find(p, pl, tag, &vlen);
    if (!r || vlen < 2) return false;
    uint16_t nv; memcpy(&nv, r, 2); *v = ntohs(nv);
    return true;
}

bool tlv_read_u32(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint32_t *v) {
    uint16_t vlen;
    const uint8_t *r = tlv_find(p, pl, tag, &vlen);
    if (!r || vlen < 4) return false;
    uint32_t nv; memcpy(&nv, r, 4); *v = ntohl(nv);
    return true;
}

bool tlv_read_u64(const uint8_t *p, uint32_t pl, tlv_tag_t tag, uint64_t *v) {
    uint16_t vlen;
    const uint8_t *r = tlv_find(p, pl, tag, &vlen);
    if (!r || vlen < 8) return false;
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) val = (val << 8) | r[i];
    *v = val;
    return true;
}
