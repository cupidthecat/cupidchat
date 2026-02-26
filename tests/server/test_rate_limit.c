/*
 * tests/server/test_rate_limit.c — unit tests for token-bucket rate limiter
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* include the server conn header to get rate_bucket_t */
#include "server/conn.h"
#include "server/rate_limit.h"
#include "server/config.h"

static int failures = 0;
#define PASS(n)       printf("  PASS  %s\n", n)
#define FAIL(n, ...)  do { printf("  FAIL  %s: ", n); printf(__VA_ARGS__); printf("\n"); failures++; } while(0)

static void test_burst_allowed(void) {
    const char *name = "burst messages allowed";
    rate_bucket_t b;
    rate_bucket_init(&b, 10, 5);   /* 10 msg/s, burst=5 */

    int allowed = 0;
    for (int i = 0; i < 5; i++)
        if (rate_consume(&b)) allowed++;

    if (allowed != 5) FAIL(name, "expected 5, got %d", allowed);
    else PASS(name);
}

static void test_burst_exceeded(void) {
    const char *name = "burst exceeded blocks";
    rate_bucket_t b;
    rate_bucket_init(&b, 10, 3);

    for (int i = 0; i < 3; i++) rate_consume(&b);   /* drain */

    if (rate_consume(&b)) FAIL(name, "should have been blocked");
    else PASS(name);
}

static void test_refill_over_time(void) {
    const char *name = "tokens refill over time";
    rate_bucket_t b;
    rate_bucket_init(&b, 5, 5);

    /* drain all tokens */
    for (int i = 0; i < 5; i++) rate_consume(&b);
    if (b.tokens != 0) { FAIL(name, "expected 0 tokens after drain"); return; }

    /* artificially move last_refill back 2 seconds */
    b.last_refill -= 2;
    rate_refill(&b);

    /* should have 10 tokens (5/s * 2s) but capped at capacity=5 */
    if (b.tokens != 5) FAIL(name, "expected 5 after refill, got %d", b.tokens);
    else PASS(name);
}

static void test_full_bucket_no_overflow(void) {
    const char *name = "full bucket doesn't overflow capacity";
    rate_bucket_t b;
    rate_bucket_init(&b, 100, 10);
    b.last_refill -= 100;   /* simulate 100 seconds idle */
    rate_refill(&b);
    if (b.tokens > b.capacity) FAIL(name, "tokens %d > capacity %d", b.tokens, b.capacity);
    else PASS(name);
}

int main(void) {
    printf("=== test_rate_limit ===\n");
    test_burst_allowed();
    test_burst_exceeded();
    test_refill_over_time();
    test_full_bucket_no_overflow();
    printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
