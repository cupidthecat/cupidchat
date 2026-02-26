/*
 * src/server/core/rate_limit.c -  token-bucket rate limiter
 *
 * Each connection gets a rate_bucket_t.  Callers call rate_consume() before
 * accepting a chargeable message.  A timerfd tick calls rate_refill()
 * periodically to top up tokens.
 */

#include <time.h>
#include <stdbool.h>

#include "server/rate_limit.h"
#include "server/conn.h"
#include "server/log.h"

/* Refill tokens based on elapsed wall-clock seconds. */
void rate_refill(rate_bucket_t *b) {
    time_t now   = time(NULL);
    int elapsed  = (int)(now - b->last_refill);
    if (elapsed <= 0) return;

    b->tokens += elapsed * b->rate;
    if (b->tokens > b->capacity) b->tokens = b->capacity;
    b->last_refill = now;
}

/*
 * Try to consume one token.
 * Returns true if the message is allowed, false if rate-limited.
 */
bool rate_consume(rate_bucket_t *b) {
    rate_refill(b);
    if (b->tokens <= 0) return false;
    b->tokens--;
    return true;
}

void rate_bucket_init(rate_bucket_t *b, int rate, int burst) {
    b->rate       = rate;
    b->capacity   = burst;
    b->tokens     = burst;
    b->last_refill = time(NULL);
}
