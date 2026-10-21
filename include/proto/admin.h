#pragma once
#include <stdbool.h>
#include <stdint.h>

#define POLL_OPTIONS_MAX 4

typedef struct {
    bool admin, muted, banned;
    char reason[257];
} account_controls_t;

typedef struct {
    uint64_t id;
    char question[257], options[POLL_OPTIONS_MAX][97];
    uint32_t votes[POLL_OPTIONS_MAX], my_vote;
    int option_count;
    bool closed;
} poll_info_t;
