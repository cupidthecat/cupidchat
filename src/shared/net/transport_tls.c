/*
 * src/shared/net/transport_tls.c -  OpenSSL TLS transport implementation
 *
 * Compiled only when the Makefile is invoked with TLS=1 (which adds
 * -DCUPID_TLS and replaces transport_tls_stub.c with this file).
 *
 * Design:
 *  - vtable-compatible with transport_posix.c (same transport_t struct)
 *  - Server role: SSL_accept path; Client role: SSL_connect path
 *  - TLS handshake is transparent to callers:
 *       tls_read / tls_write internally progress the handshake if
 *       the transport is still in TRANSPORT_TLS_HANDSHAKE mode,
 *       returning EAGAIN until the handshake completes.
 *  - tls_handshake() is the explicit hook used by the client's
 *       poll-loop to drive the handshake to completion before sending HELLO.
 *  - Minimum TLS version: 1.2.  The session negotiates 1.3 if possible.
 *  - Server cert verification for clients: optional (pass ca_bundle=NULL
 *       to skip -  useful for self-signed dev certs).
 *
 * Thread safety: NOT thread-safe (single-threaded server/client design).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "net/transport.h"

/* Internal TLS context */

typedef struct {
    SSL_CTX *ctx;
    SSL     *ssl;
} tls_priv_t;

/* Error helpers */

static void log_ssl_errors(const char *prefix) {
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        fprintf(stderr, "[tls] %s: %s\n", prefix, buf);
    }
}

/* Handshake helper (shared by read/write/handshake vtable entries) */

/*
 * Returns 0 = handshake complete and transport is now ESTABLISHED.
 *         1 = want read (call again when fd is readable)
 *         2 = want write (call again when fd is writable)
 *        -1 = fatal error
 */
static int do_handshake(transport_t *t) {
    tls_priv_t *priv = t->tls_ctx;
    int r = SSL_do_handshake(priv->ssl);
    if (r == 1) {
        t->mode = TRANSPORT_TLS_ESTABLISHED;
        return 0;
    }
    int err = SSL_get_error(priv->ssl, r);
    switch (err) {
        case SSL_ERROR_WANT_READ:
            t->mode = TRANSPORT_TLS_WANT_READ;
            errno = EAGAIN;
            return 1;
        case SSL_ERROR_WANT_WRITE:
            t->mode = TRANSPORT_TLS_WANT_WRITE;
            errno = EAGAIN;
            return 2;
        default:
            log_ssl_errors("SSL_do_handshake");
            return -1;
    }
}

/* vtable: read */

static ssize_t tls_read(transport_t *t, void *buf, size_t n) {
    /* Progress handshake if not yet established */
    if (t->mode != TRANSPORT_TLS_ESTABLISHED) {
        int hr = do_handshake(t);
        if (hr != 0) { errno = EAGAIN; return -1; }
    }

    tls_priv_t *priv = t->tls_ctx;
    int r = SSL_read(priv->ssl, buf, (int)n);
    if (r > 0) return (ssize_t)r;

    int err = SSL_get_error(priv->ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN || r == 0) return 0;  /* clean EOF */
    log_ssl_errors("SSL_read");
    return -1;
}

/* vtable: write */

static ssize_t tls_write(transport_t *t, const void *buf, size_t n) {
    if (t->mode != TRANSPORT_TLS_ESTABLISHED) {
        int hr = do_handshake(t);
        if (hr != 0) { errno = EAGAIN; return -1; }
    }

    tls_priv_t *priv = t->tls_ctx;
    int r = SSL_write(priv->ssl, buf, (int)n);
    if (r > 0) return (ssize_t)r;

    int err = SSL_get_error(priv->ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    log_ssl_errors("SSL_write");
    return -1;
}

/* vtable: handshake */

static int tls_handshake(transport_t *t) {
    if (t->mode == TRANSPORT_TLS_ESTABLISHED) return 0;
    return do_handshake(t);
}

/* vtable: close */

static void tls_close(transport_t *t) {
    tls_priv_t *priv = t->tls_ctx;
    if (priv) {
        if (priv->ssl) {
            SSL_shutdown(priv->ssl);
            SSL_free(priv->ssl);
            priv->ssl = NULL;
        }
        if (priv->ctx) {
            SSL_CTX_free(priv->ctx);
            priv->ctx = NULL;
        }
        free(priv);
        t->tls_ctx = NULL;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

/* Factory */

transport_t *transport_tls_new(int fd, bool server_role,
                               const char *cert, const char *key,
                               const char *ca_bundle) {
    /* OpenSSL 1.1+ auto-initialises; explicit init is a no-op but harmless */
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                   | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    const SSL_METHOD *method = server_role
                               ? TLS_server_method()
                               : TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) { log_ssl_errors("SSL_CTX_new"); return NULL; }

    /* Require TLS 1.2 minimum; 1.3 is negotiated if both sides support it */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* Disable old / weak ciphers */
    SSL_CTX_set_options(ctx,
        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
        SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 |
        SSL_OP_CIPHER_SERVER_PREFERENCE);

    if (server_role) {
        /* Server: must have cert + key */
        if (cert && *cert) {
            if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
                log_ssl_errors("use_certificate_chain_file");
                SSL_CTX_free(ctx);
                return NULL;
            }
        }
        if (key && *key) {
            if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
                log_ssl_errors("use_PrivateKey_file");
                SSL_CTX_free(ctx);
                return NULL;
            }
            if (SSL_CTX_check_private_key(ctx) != 1) {
                log_ssl_errors("check_private_key");
                SSL_CTX_free(ctx);
                return NULL;
            }
        }
        /* Optional: require client cert */
        if (ca_bundle && *ca_bundle) {
            if (SSL_CTX_load_verify_locations(ctx, ca_bundle, NULL) != 1) {
                log_ssl_errors("load_verify_locations (server)");
                SSL_CTX_free(ctx);
                return NULL;
            }
            SSL_CTX_set_verify(ctx,
                SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        }
    } else {
        /* Client: optionally verify server certificate */
        if (ca_bundle && *ca_bundle) {
            if (SSL_CTX_load_verify_locations(ctx, ca_bundle, NULL) != 1) {
                log_ssl_errors("load_verify_locations (client)");
                SSL_CTX_free(ctx);
                return NULL;
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        } else {
            /* Skip server cert verification -  for dev/self-signed scenarios */
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        }
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        log_ssl_errors("SSL_new");
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_set_fd(ssl, fd) != 1) {
        log_ssl_errors("SSL_set_fd");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (server_role)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);

    tls_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return NULL;
    }
    priv->ctx = ctx;
    priv->ssl = ssl;

    transport_t *t = calloc(1, sizeof(*t));
    if (!t) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        free(priv);
        return NULL;
    }
    t->fd        = fd;
    t->mode      = TRANSPORT_TLS_HANDSHAKE;
    t->read      = tls_read;
    t->write     = tls_write;
    t->handshake = tls_handshake;
    t->close     = tls_close;
    t->tls_ctx   = priv;
    return t;
}
