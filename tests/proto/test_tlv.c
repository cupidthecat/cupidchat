/*
 * tests/proto/test_tlv.c — unit tests for TLV encode/decode
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "proto/tlv.h"
#include "proto/types.h"

static int failures = 0;
#define PASS(n)       printf("  PASS  %s\n", n)
#define FAIL(n, ...)  do { printf("  FAIL  %s: ", n); printf(__VA_ARGS__); printf("\n"); failures++; } while(0)

static void test_str_roundtrip(void) {
    const char *name = "string roundtrip";
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, "alice");
    tlv_put_str(&b, TAG_ROOM, "general");
    uint32_t len;
    uint8_t *buf = tlv_builder_take(&b, &len);

    char nick[64] = {0}, room[64] = {0};
    if (!tlv_read_str(buf, len, TAG_NICK, nick, sizeof(nick))) { FAIL(name, "nick missing"); goto done; }
    if (!tlv_read_str(buf, len, TAG_ROOM, room, sizeof(room))) { FAIL(name, "room missing"); goto done; }
    if (strcmp(nick, "alice") != 0) { FAIL(name, "nick mismatch"); goto done; }
    if (strcmp(room, "general") != 0) { FAIL(name, "room mismatch"); goto done; }
    PASS(name);
done: free(buf);
}

static void test_integers(void) {
    const char *name = "integer types roundtrip";
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_u8 (&b, TAG_SCOPE,   0xAB);
    tlv_put_u16(&b, TAG_ERROR_CODE, 0x1234);
    tlv_put_u32(&b, TAG_USER_ID, 0xDEADBEEF);
    tlv_put_u64(&b, TAG_TIMESTAMP, 0x123456789ABCDEF0ULL);
    uint32_t len; uint8_t *buf = tlv_builder_take(&b, &len);

    uint8_t  v8  = 0;
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    if (!tlv_read_u8 (buf, len, TAG_SCOPE,      &v8))  { FAIL(name, "u8 missing");  goto done; }
    if (!tlv_read_u16(buf, len, TAG_ERROR_CODE, &v16)) { FAIL(name, "u16 missing"); goto done; }
    if (!tlv_read_u32(buf, len, TAG_USER_ID,    &v32)) { FAIL(name, "u32 missing"); goto done; }
    if (!tlv_read_u64(buf, len, TAG_TIMESTAMP,  &v64)) { FAIL(name, "u64 missing"); goto done; }
    if (v8  != 0xAB)               { FAIL(name, "u8 mismatch");  goto done; }
    if (v16 != 0x1234)             { FAIL(name, "u16 mismatch"); goto done; }
    if (v32 != 0xDEADBEEF)        { FAIL(name, "u32 mismatch"); goto done; }
    if (v64 != 0x123456789ABCDEF0ULL) { FAIL(name, "u64 mismatch"); goto done; }
    PASS(name);
done: free(buf);
}

static void test_missing_tag(void) {
    const char *name = "missing tag returns false";
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, "bob");
    uint32_t len; uint8_t *buf = tlv_builder_take(&b, &len);

    char room[32] = {0};
    if (tlv_read_str(buf, len, TAG_ROOM, room, sizeof(room)))
        FAIL(name, "should have returned false");
    else PASS(name);
    free(buf);
}

static void test_iterator(void) {
    const char *name = "TLV iterator order";
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, "a");
    tlv_put_str(&b, TAG_NICK, "b");
    tlv_put_str(&b, TAG_NICK, "c");
    uint32_t len; uint8_t *buf = tlv_builder_take(&b, &len);

    tlv_reader_t r; tlv_reader_init(&r, buf, len);
    tlv_field_t f;
    int count = 0;
    const char *expected[] = {"a", "b", "c"};
    while (tlv_reader_next(&r, &f)) {
        if (f.tag != TAG_NICK || f.len != 1 || f.val[0] != expected[count][0]) {
            FAIL(name, "mismatch at %d", count); free(buf); return;
        }
        count++;
    }
    if (count != 3) FAIL(name, "expected 3, got %d", count);
    else PASS(name);
    free(buf);
}

static void test_empty_string(void) {
    const char *name = "empty string tag";
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_MOTD, "");
    uint32_t len; uint8_t *buf = tlv_builder_take(&b, &len);
    char motd[8] = {0x7F, 0x7F, 0};
    if (!tlv_read_str(buf, len, TAG_MOTD, motd, sizeof(motd)))
        FAIL(name, "tag missing");
    else if (motd[0] != '\0')
        FAIL(name, "expected empty string, got '%s'", motd);
    else PASS(name);
    free(buf);
}

int main(void) {
    printf("=== test_tlv ===\n");
    test_str_roundtrip();
    test_integers();
    test_missing_tag();
    test_iterator();
    test_empty_string();
    printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
