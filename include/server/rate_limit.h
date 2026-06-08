#ifndef SERVER_RATE_LIMIT_H
#define SERVER_RATE_LIMIT_H

#include <stdbool.h>
#include <time.h>

typedef struct {
    int rate;
    int capacity;
    int tokens;
    time_t last_refill;
} rate_bucket_t;

void rate_bucket_init(rate_bucket_t *b, int rate, int burst);
void rate_refill(rate_bucket_t *b);
bool rate_consume(rate_bucket_t *b);

#endif
