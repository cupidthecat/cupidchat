/*
 * tests/proto/test_frame.c — unit tests for frame encode/decode
 *
 * Minimal test harness (no external dependencies).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "proto/frame.h"
#include "proto/types.h"

#define PASS(name) printf("  PASS  %s\n", name)
#define FAIL(name, ...) do { \
    printf("  FAIL  %s: ", name); printf(__VA_ARGS__); printf("\n"); \
    failures++; \
} while(0)

static int failures = 0;

/* ── Test: round-trip encode / decode ───────────────────────────────────── */
static void test_roundtrip(void) {
    const char *name = "roundtrip encode/decode";
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0xAB, 0xCD};
    uint8_t *buf = NULL;
    ssize_t blen = frame_encode(0x0012, 0x0002, 42,
                                payload, sizeof(payload), &buf);
    if (blen < 0 || !buf) { FAIL(name, "encode returned %zd", blen); return; }

    frame_parser_t p;
    frame_parser_init(&p);
    frame_parser_push(&p, buf, (size_t)blen);
    free(buf);

    frame_t f;
    parse_result_t pr = frame_parser_next(&p, &f);
    if (pr != PARSE_FRAME_OK) { FAIL(name, "parse_result=%d", pr); goto done; }

    if (f.hdr.type != 0x0012) { FAIL(name, "type mismatch"); goto cleanup; }
    if (f.hdr.flags != 0x0002) { FAIL(name, "flags mismatch"); goto cleanup; }
    if (f.hdr.seq != 42) { FAIL(name, "seq mismatch"); goto cleanup; }
    if (f.hdr.length != sizeof(payload)) { FAIL(name, "length mismatch"); goto cleanup; }
    if (memcmp(f.payload, payload, sizeof(payload)) != 0) {
        FAIL(name, "payload mismatch"); goto cleanup;
    }
    PASS(name);

cleanup:
    frame_free(&f);
done:
    frame_parser_free(&p);
}

/* ── Test: incremental push (byte-at-a-time) ────────────────────────────── */
static void test_incremental(void) {
    const char *name = "incremental byte-at-a-time";
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t *buf = NULL;
    ssize_t blen = frame_encode(0x0001, 0, 1, payload, sizeof(payload), &buf);
    assert(blen > 0 && buf);

    frame_parser_t p;
    frame_parser_init(&p);

    frame_t f;
    parse_result_t pr = PARSE_NEED_MORE;
    for (ssize_t i = 0; i < blen - 1; i++) {
        frame_parser_push(&p, buf + i, 1);
        pr = frame_parser_next(&p, &f);
        if (pr != PARSE_NEED_MORE) { FAIL(name, "expected NEED_MORE at byte %zd", i); goto done; }
    }
    frame_parser_push(&p, buf + blen - 1, 1);
    pr = frame_parser_next(&p, &f);
    if (pr != PARSE_FRAME_OK) { FAIL(name, "expected FRAME_OK, got %d", pr); goto done; }
    if (f.hdr.type != 0x0001) { FAIL(name, "type mismatch"); frame_free(&f); goto done; }
    PASS(name);
    frame_free(&f);
done:
    free(buf);
    frame_parser_free(&p);
}

/* ── Test: oversized payload rejection ──────────────────────────────────── */
static void test_oversize(void) {
    const char *name = "oversized payload rejected";
    /* manually craft a frame with length > FRAME_MAX_PAYLOAD */
    uint8_t hdr[FRAME_HEADER_SIZE] = {0};
    uint32_t big = FRAME_MAX_PAYLOAD + 1;
    hdr[0] = (big >> 24) & 0xFF;
    hdr[1] = (big >> 16) & 0xFF;
    hdr[2] = (big >>  8) & 0xFF;
    hdr[3] = (big >>  0) & 0xFF;

    frame_parser_t p;
    frame_parser_init(&p);
    frame_parser_push(&p, hdr, sizeof(hdr));

    frame_t f;
    parse_result_t pr = frame_parser_next(&p, &f);
    if (pr != PARSE_ERROR) { FAIL(name, "expected PARSE_ERROR, got %d", pr); }
    else PASS(name);

    frame_parser_free(&p);
}

/* ── Test: multiple frames back-to-back ─────────────────────────────────── */
static void test_multi_frame(void) {
    const char *name = "multiple frames in one push";
    uint8_t *bigbuf = NULL;
    size_t total_size = 0;

    /* build 5 frames into one buffer */
    for (int i = 0; i < 5; i++) {
        uint8_t pl[4] = {(uint8_t)i, (uint8_t)(i*2), 0, 0};
        uint8_t *fb = NULL;
        ssize_t fl = frame_encode(0x0010, 0, (uint32_t)i, pl, sizeof(pl), &fb);
        bigbuf = realloc(bigbuf, total_size + (size_t)fl);
        memcpy(bigbuf + total_size, fb, (size_t)fl);
        total_size += (size_t)fl;
        free(fb);
    }

    frame_parser_t p;
    frame_parser_init(&p);
    frame_parser_push(&p, bigbuf, total_size);
    free(bigbuf);

    int count = 0;
    frame_t f;
    while (frame_parser_next(&p, &f) == PARSE_FRAME_OK) {
        frame_free(&f);
        count++;
    }

    if (count != 5) FAIL(name, "expected 5 frames, got %d", count);
    else PASS(name);

    frame_parser_free(&p);
}

/* ── Test: empty payload ────────────────────────────────────────────────── */
static void test_empty_payload(void) {
    const char *name = "empty payload frame";
    uint8_t *buf = NULL;
    ssize_t blen = frame_encode(0x8030, 0, 0, NULL, 0, &buf);
    assert(blen == FRAME_HEADER_SIZE && buf);

    frame_parser_t p; frame_parser_init(&p);
    frame_parser_push(&p, buf, (size_t)blen);
    free(buf);

    frame_t f;
    parse_result_t pr = frame_parser_next(&p, &f);
    if (pr != PARSE_FRAME_OK) { FAIL(name, "parse failed"); goto done; }
    if (f.hdr.length != 0 || f.payload != NULL) { FAIL(name, "payload not null/zero"); }
    else PASS(name);
    frame_free(&f);
done:
    frame_parser_free(&p);
}

int main(void) {
    printf("=== test_frame ===\n");
    test_roundtrip();
    test_incremental();
    test_oversize();
    test_multi_frame();
    test_empty_payload();
    printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
