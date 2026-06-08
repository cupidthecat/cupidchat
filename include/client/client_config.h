#ifndef CLIENT_CONFIG_H
#define CLIENT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "proto/types.h"

typedef struct {
    char host[64];
    uint16_t port;
    char nick[MAX_NICK_LEN + 1];
    bool use_tls;
    bool verbose;
} client_config_t;

void client_config_usage(const char *prog);
int client_config_parse(int argc, char **argv, client_config_t *cfg);

#endif
