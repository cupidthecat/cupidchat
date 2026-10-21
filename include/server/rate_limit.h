/* CupidChat public interface. */
#pragma once
#include <stdbool.h>
#include <time.h>
typedef struct { int capacity, tokens, rate; time_t last_refill; } rate_bucket_t;

void rate_refill(rate_bucket_t *b);
bool rate_consume(rate_bucket_t *b);
void rate_bucket_init(rate_bucket_t *b, int rate, int burst);
