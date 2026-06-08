#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT_PLAIN 5555
#define DEFAULT_PORT_TLS 5556
#define DEFAULT_MAX_CLIENTS 1024
#define DEFAULT_PING_INTERVAL 30
#define DEFAULT_PING_TIMEOUT 15
#define DEFAULT_RATE_MSGS 10
#define DEFAULT_RATE_BURST 25
#define DEFAULT_OBUF_LIMIT 4194304
#define DEFAULT_HISTORY_SIZE 50
#define DEFAULT_DB_PATH "cupidchat.db"

typedef struct {
    char host[64];
    uint16_t port_plain;
    uint16_t port_tls;
    bool tls_enabled;
    char tls_cert[256];
    char tls_key[256];
    char tls_ca[256];
    int max_clients;
    int ping_interval;
    int ping_timeout;
    int rate_msgs_per_sec;
    int rate_burst;
    size_t obuf_limit;
    int history_size;
    char db_path[256];
    bool verbose;
} server_config_t;

void config_defaults(server_config_t *cfg);
void config_usage(const char *prog);
int config_parse(int argc, char **argv, server_config_t *cfg);

#endif
