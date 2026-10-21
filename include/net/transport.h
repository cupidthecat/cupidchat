/* CupidChat public interface. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    TRANSPORT_PLAIN, TRANSPORT_TLS_HANDSHAKE, TRANSPORT_TLS_WANT_READ,
    TRANSPORT_TLS_WANT_WRITE, TRANSPORT_TLS_ESTABLISHED
} transport_mode_t;
typedef struct transport transport_t;
struct transport {
    int fd;
    transport_mode_t mode;
    void *tls_ctx;
    ssize_t (*read)(transport_t *, void *, size_t);
    ssize_t (*write)(transport_t *, const void *, size_t);
    int (*handshake)(transport_t *);
    void (*close)(transport_t *);
};
transport_t *transport_tls_new(int fd, bool server_role,
    const char *cert, const char *key, const char *ca_bundle);

transport_t *transport_plain_new(int fd);
void transport_free(transport_t *t);
int transport_set_nonblocking(int fd);
