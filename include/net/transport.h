#ifndef NET_TRANSPORT_H
#define NET_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>

typedef enum {
    TRANSPORT_PLAIN = 0,
    TRANSPORT_TLS
} transport_mode_t;

typedef struct transport_t transport_t;
struct transport_t {
    int fd;
    transport_mode_t mode;
    void *tls_ctx;
    
    ssize_t (*read)(transport_t *t, void *buf, size_t n);
    ssize_t (*write)(transport_t *t, const void *buf, size_t n);
    int (*handshake)(transport_t *t);
    void (*close)(transport_t *t);
};

transport_t *transport_plain_new(int fd);
void transport_free(transport_t *t);
int transport_set_nonblocking(int fd);

transport_t *transport_tls_new(int fd, bool server_role, const char *cert_path, const char *key_path, const char *ca_path);

#endif
