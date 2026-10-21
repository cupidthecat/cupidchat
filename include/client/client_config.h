/* CupidChat public interface. */
#pragma once
#include "proto/types.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char host[256], nick[MAX_NICK_LEN + 1];
    uint16_t port;
    bool use_tls, verbose;
} client_config_t;

void client_config_usage(const char *prog);
int client_config_parse(int argc, char **argv, client_config_t *cfg);
