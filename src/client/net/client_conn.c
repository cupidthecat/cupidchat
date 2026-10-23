/*
 * src/client/net/client_conn.c -  client-side network I/O
 *
 * The client uses a blocking connect then sets the socket non-blocking.
 * The main loop polls the socket fd with poll().  The connection feeds all
 * received bytes through the shared frame_parser and emits complete frames
 * to the model.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client/client_conn.h"
#include "client/text.h"
#include "net/transport.h"
#include "proto/frame.h"
#include "proto/tlv.h"
#include "proto/types.h"

/* Connect */

struct client_output {
    uint8_t *data;
    size_t length, sent;
    client_output_t *next;
};

#define CLIENT_OUTPUT_LIMIT (256u * 1024u)

static void free_output(client_output_t *output) {
    explicit_bzero(output->data, output->length);
    free(output->data);
    free(output);
}

int client_connect(client_conn_t *cc, const char *host, uint16_t port, bool use_tls) {
    memset(cc, 0, sizeof(*cc));
    cc->fd = -1;
    cc->seq = 1;

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(fd);
        return -1;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* switch to non-blocking after connect */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    transport_t *t;
    if (use_tls) {
        t = transport_tls_new(fd, false, NULL, NULL, NULL);
        if (!t) {
            close(fd);
            return -1;
        }

        /* Drive TLS handshake to completion before the main loop takes over.
         * The fd is already non-blocking, so we poll for readability /
         * writability as the handshake dictates. 5-second total timeout. */
        const int TLS_HANDSHAKE_TIMEOUT_MS = 5000;
        int elapsed = 0;
        while (1) {
            int hr = t->handshake(t);
            if (hr == 0)
                break; /* established */
            if (hr < 0) {
                fprintf(stderr, "TLS handshake failed\n");
                transport_free(t);
                return -1;
            }
            if (elapsed >= TLS_HANDSHAKE_TIMEOUT_MS) {
                fprintf(stderr, "TLS handshake timeout\n");
                transport_free(t);
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = (hr == 1) ? POLLIN : POLLOUT};
            int pr = poll(&pfd, 1, 100);
            if (pr < 0 && errno != EINTR) {
                transport_free(t);
                return -1;
            }
            elapsed += 100;
        }
        fprintf(stderr, "TLS handshake complete\n");
    } else {
        t = transport_plain_new(fd);
        if (!t) {
            close(fd);
            return -1;
        }
    }

    cc->fd = fd;
    cc->transport = t;
    cc->connected = true;

    if (frame_parser_init(&cc->parser) < 0) {
        transport_free(t);
        cc->transport = NULL;
        cc->connected = false;
        cc->fd = -1;
        return -1;
    }

    return 0;
}

void client_disconnect(client_conn_t *cc) {
    if (!cc)
        return;
    frame_parser_free(&cc->parser);
    while (cc->output_head) {
        client_output_t *next = cc->output_head->next;
        free_output(cc->output_head);
        cc->output_head = next;
    }
    cc->output_tail = NULL;
    cc->output_bytes = 0;
    if (cc->transport) {
        transport_free(cc->transport);
        cc->transport = NULL;
    }
    cc->fd = -1;
    cc->connected = false;
}

/* Send a frame */

int client_send_frame(client_conn_t *cc, uint16_t type, const uint8_t *payload, uint32_t plen) {
    if (!cc->connected || cc->send_failed)
        return -1;
    uint8_t *buf;
    ssize_t blen = frame_encode(type, FRAME_FLAG_PRIORITY_HIGH, cc->seq++, payload, plen, &buf);
    if (blen < 0) {
        cc->send_failed = true;
        return -1;
    }
    if ((size_t)blen > CLIENT_OUTPUT_LIMIT - cc->output_bytes) {
        explicit_bzero(buf, (size_t)blen);
        free(buf);
        cc->send_failed = true;
        return -1;
    }
    client_output_t *output = calloc(1, sizeof(*output));
    if (!output) {
        explicit_bzero(buf, (size_t)blen);
        free(buf);
        cc->send_failed = true;
        return -1;
    }
    output->data = buf;
    output->length = (size_t)blen;
    if (cc->output_tail)
        cc->output_tail->next = output;
    else
        cc->output_head = output;
    cc->output_tail = output;
    cc->output_bytes += (size_t)blen;
    return client_flush(cc);
}

int client_flush(client_conn_t *cc) {
    if (!cc->connected || cc->send_failed)
        return -1;
    size_t flushed = 0;
    while (cc->output_head && flushed < 65536) {
        client_output_t *output = cc->output_head;
        size_t remaining = output->length - output->sent;
        if (cc->transport->mode == TRANSPORT_PLAIN && remaining > 65536 - flushed)
            remaining = 65536 - flushed;
        ssize_t n = cc->transport->write(cc->transport, output->data + output->sent, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            cc->send_failed = true;
            return -1;
        }
        if (!n) {
            cc->send_failed = true;
            return -1;
        }
        output->sent += (size_t)n;
        flushed += (size_t)n;
        if (output->sent == output->length) {
            cc->output_head = output->next;
            if (!cc->output_head)
                cc->output_tail = NULL;
            cc->output_bytes -= output->length;
            free_output(output);
        }
    }
    return 0;
}

/* Helpers for common message types */

int client_send_hello(client_conn_t *cc, const char *nick) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u16(&b, TAG_PROTO_VERSION, CUPID_PROTO_VERSION);
    tlv_put_str(&b, TAG_NICK, nick);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_HELLO, p, plen);
    free(p);
    return r;
}

int client_send_nick(client_conn_t *cc, const char *nick) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, nick);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_NICK, p, plen);
    free(p);
    return r;
}

int client_send_register(client_conn_t *cc, const char *nick, const char *pass) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, nick);
    tlv_put_str(&b, TAG_PASSWORD, pass);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_REGISTER, p, plen);
    free(p);
    return r;
}

int client_send_login(client_conn_t *cc, const char *nick, const char *pass) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, nick);
    tlv_put_str(&b, TAG_PASSWORD, pass);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_LOGIN, p, plen);
    free(p);
    return r;
}

int client_send_logout(client_conn_t *cc) {
    return client_send_frame(cc, CMSG_LOGOUT, NULL, 0);
}

int client_send_away(client_conn_t *cc, const char *msg) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    if (msg && msg[0])
        tlv_put_str(&b, TAG_AWAY_MSG, msg);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_AWAY, p, plen);
    free(p);
    return r;
}

int client_send_typing(client_conn_t *cc, const char *room) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_TYPING, p, plen);
    free(p);
    return r;
}

int client_send_join(client_conn_t *cc, const char *room) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_JOIN, p, plen);
    free(p);
    return r;
}

int client_send_leave(client_conn_t *cc, const char *room) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_LEAVE, p, plen);
    free(p);
    return r;
}

int client_send_room_msg(client_conn_t *cc, const char *room, const char *text) {
    char expanded[MAX_MSG_LEN + 1];
    if (text_expand_emoji(text, expanded, sizeof(expanded)) < 0)
        return -1;
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    tlv_put_str(&b, TAG_TEXT, expanded);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_ROOM_MSG, p, plen);
    free(p);
    return r;
}

/* Sends an IRC/CTCP-style action message (rendered as: * nick text)
 * The text is wrapped in \x01ACTION ...\x01 before sending so the server
 * stores it verbatim and echoes it back to all members including history. */
int client_send_room_action(client_conn_t *cc, const char *room, const char *text) {
    char wrapped[MAX_MSG_LEN + 1];
    int length = snprintf(wrapped, sizeof(wrapped),
                          "\x01"
                          "ACTION %s"
                          "\x01",
                          text);
    if (length < 0 || length >= (int)sizeof(wrapped))
        return -1;
    return client_send_room_msg(cc, room, wrapped);
}

int client_send_set_topic(client_conn_t *cc, const char *room, const char *topic) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    tlv_put_str(&b, TAG_TOPIC, topic);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_SET_TOPIC, p, plen);
    free(p);
    return r;
}

int client_send_dm(client_conn_t *cc, const char *target_nick, const char *text) {
    return client_send_dm_to(cc, target_nick, text, 0);
}

int client_send_dm_to(client_conn_t *cc, const char *target_nick, const char *text, uint32_t uid) {
    char expanded[MAX_MSG_LEN + 1];
    if (text_expand_emoji(text, expanded, sizeof(expanded)) < 0)
        return -1;
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK, target_nick);
    if (uid)
        tlv_put_u32(&b, TAG_PEER_UID, uid);
    tlv_put_str(&b, TAG_TEXT, expanded);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_DM, p, plen);
    free(p);
    return r;
}

int client_send_list_rooms(client_conn_t *cc) {
    return client_send_frame(cc, CMSG_LIST_ROOMS, NULL, 0);
}

int client_send_delete_room(client_conn_t *cc, const char *room) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    uint32_t plen;
    uint8_t *p = tlv_builder_take(&b, &plen);
    int r = client_send_frame(cc, CMSG_DELETE_ROOM, p, plen);
    free(p);
    return r;
}

int client_send_list_users(client_conn_t *cc, const char *room) {
    if (room && *room) {
        tlv_builder_t b;
        tlv_builder_init(&b);
        tlv_put_str(&b, TAG_ROOM, room);
        uint32_t plen;
        uint8_t *p = tlv_builder_take(&b, &plen);
        int r = client_send_frame(cc, CMSG_LIST_USERS, p, plen);
        free(p);
        return r;
    }
    return client_send_frame(cc, CMSG_LIST_USERS, NULL, 0);
}

int client_send_pong(client_conn_t *cc) {
    return client_send_frame(cc, CMSG_PONG, NULL, 0);
}

int client_send_buddy(client_conn_t *cc, uint16_t type, const char *account) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    if (account)
        tlv_put_str(&b, TAG_BASE_NICK, account);
    int rc = client_send_frame(cc, type, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}

/* Read incoming frames (non-blocking, call from poll loop) */

/* Returns:
 *   0   -  would block or yielded; read_pending requests another turn
 *   1   -  server closed the connection cleanly (FIN / n==0)
 *  -1   -  I/O error or framing error
 */
int client_read_frames(client_conn_t *cc, void (*on_frame)(const frame_t *, void *ctx), void *ctx) {
    uint8_t buf[65536];
    size_t bytes = 0;
    int frames = 0;
    cc->read_pending = false;

    while (1) {
        frame_t f;
        parse_result_t pr = frame_parser_next(&cc->parser, &f);
        if (pr == PARSE_FRAME_OK) {
            on_frame(&f, ctx);
            frame_free(&f);
            if (++frames >= 256) {
                cc->read_pending = true;
                return 0;
            }
            continue;
        }
        if (pr == PARSE_ERROR)
            return -1;
        /* Give keyboard input and rendering a turn during incoming bursts. */
        if (bytes >= sizeof(buf)) {
            cc->read_pending = true;
            return 0;
        }
        ssize_t n = cc->transport->read(cc->transport, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1; /* I/O error */
        }
        if (n == 0)
            return 1; /* server closed cleanly */

        if (frame_parser_push(&cc->parser, buf, (size_t)n) < 0)
            return -1;
        bytes += (size_t)n;
    }
}

int client_get_profile(client_conn_t *cc, uint32_t request, uint32_t uid, const char *account,
                       const char *nick) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    if (uid)
        tlv_put_u32(&b, TAG_USER_ID, uid);
    if (account && *account)
        tlv_put_str(&b, TAG_BASE_NICK, account);
    else if (nick && *nick)
        tlv_put_str(&b, TAG_NICK, nick);
    int rc = client_send_frame(cc, CMSG_PROFILE_GET, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}

int client_save_profile(client_conn_t *cc, uint32_t request, const profile_fields_t *fields) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    int rc = -1;
    if (tlv_put_u32(&b, TAG_REQUEST_ID, request) == 0 && profile_fields_write(&b, fields) == 0)
        rc = client_send_frame(cc, CMSG_PROFILE_SET, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}

int client_send_admin(client_conn_t *cc, const char *action, const char *account,
                      const char *body) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    int rc = -1;
    if (tlv_put_str(&b, TAG_ADMIN_ACTION, action) == 0 &&
        (!account || tlv_put_str(&b, TAG_BASE_NICK, account) == 0) &&
        tlv_put_str(&b, TAG_TEXT, body ? body : "") == 0)
        rc = client_send_frame(cc, CMSG_ADMIN, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}

int client_send_poll(client_conn_t *cc, uint64_t id, uint32_t choice) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    int rc = -1;
    if (tlv_put_u64(&b, TAG_POLL_ID, id) == 0 && tlv_put_u32(&b, TAG_VOTE, choice) == 0)
        rc = client_send_frame(cc, choice ? CMSG_POLL_VOTE : CMSG_POLL_GET, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}

int client_send_board(client_conn_t *cc, const board_request_t *r) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    bool failed = tlv_put_u32(&b, TAG_REQUEST_ID, r->request) < 0 ||
                  tlv_put_u32(&b, TAG_OFFSET, r->offset) < 0 ||
                  tlv_put_u64(&b, TAG_THREAD_ID, r->thread) < 0 ||
                  tlv_put_u64(&b, TAG_POST_ID, r->post) < 0 ||
                  tlv_put_u64(&b, TAG_BEFORE_ID, r->before) < 0 ||
                  tlv_put_u64(&b, TAG_AFTER_ID, r->after) < 0;
    if (r->title)
        failed |= tlv_put_str(&b, TAG_TITLE, r->title) < 0;
    if (r->body)
        failed |= tlv_put_str(&b, TAG_TEXT, r->body) < 0;
    if (r->setting) {
        failed |= tlv_put_str(&b, TAG_SCOPE, r->setting) < 0;
        failed |=
            tlv_put_u8(&b, !strcmp(r->setting, "pin") ? TAG_PINNED : TAG_LOCKED, r->value) < 0;
    }
    int rc = failed ? -1 : client_send_frame(cc, r->type, b.buf, (uint32_t)b.len);
    tlv_builder_free(&b);
    return rc;
}
