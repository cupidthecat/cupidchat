/*
 * src/shared/net/transport_tls_stub.c
 *
 * Compile-time stub used when TLS=1 is NOT set.
 * Provides the same symbol transport_tls_new() but always returns NULL and
 * logs a warning so callers can handle gracefully.
 * When TLS=1, this file is excluded from the build and transport_tls.c is
 * compiled instead.
 */

#include <stdio.h>
#include <stdbool.h>
#include "net/transport.h"

transport_t *transport_tls_new(int fd, bool server_role,
                               const char *cert, const char *key,
                               const char *ca_bundle) {
    (void)fd; (void)server_role; (void)cert; (void)key; (void)ca_bundle;
    fprintf(stderr,
        "[transport] TLS requested but not compiled in (rebuild with TLS=1)\n");
    return NULL;
}
