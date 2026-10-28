/*
 * src/server/util/config.c -  CLI argument parsing for cupid-chatd
 *
 * Usage:
 *   cupid-chatd [--host HOST] [--port PORT] [--tls-port PORT]
 *               [--cert FILE] [--key FILE] [--ca FILE]
 *               [--max-clients N] [--ping-interval S] [--ping-timeout S]
 *               [--rate-msgs N] [--rate-burst N] [--obuf-limit BYTES]
 *               [--history N] [--verbose] [-h|--help]
 */

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server/config.h"

/* A configured but unreadable/empty secret must never silently unlock a server. */
static int load_server_password(server_config_t *cfg, const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat st;
    if (fd < 0) {
        fprintf(stderr, "Could not open server password file\n");
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        fprintf(stderr, "Server password file must be a regular file\n");
        return -1;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return -1;
    }
    char secret[258] = {0};
    size_t n = fread(secret, 1, sizeof(secret), file);
    bool failed = ferror(file) || n == sizeof(secret);
    fclose(file);
    if (n && secret[n - 1] == '\n') {
        n--;
        if (n && secret[n - 1] == '\r')
            n--;
    }
    if (!n || n >= sizeof(cfg->server_password) || memchr(secret, 0, n) ||
        memchr(secret, '\n', n) || memchr(secret, '\r', n))
        failed = true;
    if (!failed)
        memcpy(cfg->server_password, secret, n);
    explicit_bzero(secret, sizeof(secret));
    if (failed)
        fprintf(stderr, "Server password file must contain one password of 1-255 bytes\n");
    return failed ? -1 : 0;
}

void config_defaults(server_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host, DEFAULT_HOST, sizeof(cfg->host) - 1);
    cfg->port_plain = DEFAULT_PORT_PLAIN;
    cfg->port_tls = DEFAULT_PORT_TLS;
    cfg->tls_enabled = false;
    cfg->max_clients = DEFAULT_MAX_CLIENTS;
    cfg->ping_interval = DEFAULT_PING_INTERVAL;
    cfg->ping_timeout = DEFAULT_PING_TIMEOUT;
    cfg->rate_msgs_per_sec = DEFAULT_RATE_MSGS;
    cfg->rate_burst = DEFAULT_RATE_BURST;
    cfg->obuf_limit = DEFAULT_OBUF_LIMIT;
    cfg->history_size = DEFAULT_HISTORY_SIZE;
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", DEFAULT_DB_PATH);
    cfg->verbose = false;
}

void config_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --host HOST         Listen address (default: 0.0.0.0)\n"
            "  --port PORT         Plaintext port (default: 5555)\n"
            "  --tls-port PORT     TLS port (default: 5556; requires --cert/--key)\n"
            "  --cert FILE         TLS server certificate (PEM)\n"
            "  --key  FILE         TLS private key (PEM)\n"
            "  --ca   FILE         CA bundle for client cert verify (optional)\n"
            "  --max-clients N     Max simultaneous clients (default: 1024)\n"
            "  --ping-interval S   Idle ping interval in seconds (default: 30)\n"
            "  --ping-timeout S    Seconds to wait for PONG (default: 15)\n"
            "  --rate-msgs N       Messages per second per client (default: 10)\n"
            "  --rate-burst N      Burst token count (default: 25)\n"
            "  --obuf-limit BYTES  Output queue limit per client (default: 4194304)\n"
            "  --history N         Room history messages to keep (default: 50)\n"
            "  --db PATH           SQLite database file (default: cupidchat.db)\n"
            "  --server-password-file FILE  Require a shared password before account access\n"
            "                      Or set CUPIDCHAT_SERVER_PASSWORD_FILE\n"
            "  --verbose           Enable debug logging\n"
            "  -h, --help          Show this help\n",
            prog);
}

int config_parse(int argc, char **argv, server_config_t *cfg) {
    config_defaults(cfg);

    static const struct option longopts[] = {
        {"host", required_argument, NULL, 'H'},
        {"port", required_argument, NULL, 'p'},
        {"tls-port", required_argument, NULL, 'P'},
        {"cert", required_argument, NULL, 'c'},
        {"key", required_argument, NULL, 'k'},
        {"ca", required_argument, NULL, 'C'},
        {"max-clients", required_argument, NULL, 'm'},
        {"ping-interval", required_argument, NULL, 'i'},
        {"ping-timeout", required_argument, NULL, 't'},
        {"rate-msgs", required_argument, NULL, 'r'},
        {"rate-burst", required_argument, NULL, 'b'},
        {"obuf-limit", required_argument, NULL, 'o'},
        {"history", required_argument, NULL, 'n'},
        {"db", required_argument, NULL, 'd'},
        {"server-password-file", required_argument, NULL, 's'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    const char *password_file = getenv("CUPIDCHAT_SERVER_PASSWORD_FILE");
    int opt;
    while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (opt) {
            case 'H':
                strncpy(cfg->host, optarg, sizeof(cfg->host) - 1);
                break;
            case 'p':
                cfg->port_plain = (uint16_t)atoi(optarg);
                break;
            case 'P':
                cfg->port_tls = (uint16_t)atoi(optarg);
                break;
            case 'c':
                strncpy(cfg->tls_cert, optarg, sizeof(cfg->tls_cert) - 1);
                cfg->tls_enabled = true;
                break;
            case 'k':
                strncpy(cfg->tls_key, optarg, sizeof(cfg->tls_key) - 1);
                break;
            case 'C':
                strncpy(cfg->tls_ca, optarg, sizeof(cfg->tls_ca) - 1);
                break;
            case 'm':
                cfg->max_clients = atoi(optarg);
                break;
            case 'i':
                cfg->ping_interval = atoi(optarg);
                break;
            case 't':
                cfg->ping_timeout = atoi(optarg);
                break;
            case 'r':
                cfg->rate_msgs_per_sec = atoi(optarg);
                break;
            case 'b':
                cfg->rate_burst = atoi(optarg);
                break;
            case 'o':
                cfg->obuf_limit = (size_t)atol(optarg);
                break;
            case 'n':
                cfg->history_size = atoi(optarg);
                break;
            case 'd':
                snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", optarg);
                break;
            case 's':
                password_file = optarg;
                if (!*password_file) {
                    fprintf(stderr, "Server password file path cannot be empty\n");
                    return -1;
                }
                break;
            case 'v':
                cfg->verbose = true;
                break;
            case 'h':
                return 1;
            default:
                return -1;
        }
    }

    /* Clamp all integer options to sane ranges so downstream code never
     * sees 0 or negative values that could cause division by zero or
     * mis-sized allocations. */
    if (cfg->max_clients < 1)
        cfg->max_clients = 1;
    if (cfg->max_clients > 65535)
        cfg->max_clients = 65535;
    if (cfg->ping_interval < 1)
        cfg->ping_interval = 1;
    if (cfg->ping_timeout < 1)
        cfg->ping_timeout = 1;
    if (cfg->rate_msgs_per_sec < 1)
        cfg->rate_msgs_per_sec = 1;
    if (cfg->rate_burst < 1)
        cfg->rate_burst = 1;
    if (cfg->obuf_limit < 4096)
        cfg->obuf_limit = 4096;
    if (cfg->history_size < 1)
        cfg->history_size = 1;
    if (cfg->history_size > 10000)
        cfg->history_size = 10000;
    if (cfg->port_plain == 0)
        cfg->port_plain = DEFAULT_PORT_PLAIN;
    if (cfg->port_tls == 0)
        cfg->port_tls = DEFAULT_PORT_TLS;
    /* ping_timeout must be less than ping_interval to be meaningful */
    if (cfg->ping_timeout >= cfg->ping_interval)
        cfg->ping_timeout = cfg->ping_interval - 1;
    if (cfg->ping_timeout < 1)
        cfg->ping_timeout = 1;

    if (password_file && *password_file && load_server_password(cfg, password_file) < 0)
        return -1;
    return 0;
}
