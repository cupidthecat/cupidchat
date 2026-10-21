/* CupidChat public interface. */
#pragma once
#include <stdint.h>

#define CUPID_PROTO_VERSION 1
#define CUPID_SERVER_NAME "CupidChat"
#define MAX_NICK_LEN 32
#define MAX_ROOM_LEN 64
#define MAX_MSG_LEN 4096
#define CAP_HISTORY 0x00000001u

/* Shared wire values. Rebuild clients with these recovered headers:
 * only HELLO, WELCOME, ERROR, version and nick tags survive in the
 * original smoke script; the remaining assignments are newly defined. */
typedef enum {
    CMSG_HELLO = 0x0001,
    CMSG_NICK = 0x0002,
    CMSG_REGISTER = 0x0003,
    CMSG_LOGIN = 0x0004,
    CMSG_LOGOUT = 0x0005,
    CMSG_AWAY = 0x0006,
    CMSG_JOIN = 0x0010,
    CMSG_LEAVE = 0x0011,
    CMSG_ROOM_MSG = 0x0012,
    CMSG_DM = 0x0013,
    CMSG_TYPING = 0x0014,
    CMSG_SET_TOPIC = 0x0015,
    CMSG_DELETE_ROOM = 0x0016,
    CMSG_LIST_ROOMS = 0x0020,
    CMSG_LIST_USERS = 0x0021,
    CMSG_PING = 0x0030,
    CMSG_PONG = 0x0031,
    CMSG_FILE_OFFER = 0x0100,
    CMSG_FILE_ACCEPT = 0x0101,
    CMSG_FILE_DECLINE = 0x0102,
    CMSG_FILE_CHUNK = 0x0103,
    CMSG_FILE_END = 0x0104,
    CMSG_FILE_ACK = 0x0105
} client_msg_t;

typedef enum {
    SMSG_WELCOME = 0x8001,
    SMSG_ERROR = 0x8002,
    SMSG_REGISTER_OK = 0x8003,
    SMSG_LOGIN_OK = 0x8004,
    SMSG_LOGOUT_OK = 0x8005,
    SMSG_USER_AWAY = 0x8006,
    SMSG_USER_JOINED = 0x8010,
    SMSG_USER_LEFT = 0x8011,
    SMSG_ROOM_MSG = 0x8012,
    SMSG_DM = 0x8013,
    SMSG_TYPING_NOTIFY = 0x8014,
    SMSG_ROOM_TOPIC = 0x8015,
    SMSG_ROOM_DELETED = 0x8016,
    SMSG_ROOM_CREATED = 0x8017,
    SMSG_ROOM_HISTORY = 0x8018,
    SMSG_NICK_CHANGED = 0x8019,
    SMSG_ROOM_LIST = 0x8020,
    SMSG_USER_LIST = 0x8021,
    SMSG_PING = 0x8030,
    SMSG_PONG = 0x8031
} server_msg_t;

typedef enum {
    TAG_PROTO_VERSION = 0x0001,
    TAG_SESSION_ID = 0x0002,
    TAG_SERVER_NAME = 0x0003,
    TAG_MOTD = 0x0004,
    TAG_CAPS = 0x0005,
    TAG_NICK = 0x0010,
    TAG_USER_ID = 0x0011,
    TAG_OLD_NICK = 0x0012,
    TAG_BASE_NICK = 0x0013,
    TAG_PASSWORD = 0x0014,
    TAG_AWAY_MSG = 0x0015,
    TAG_ROOM = 0x0020,
    TAG_TOPIC = 0x0021,
    TAG_OWNER_UID = 0x0022,
    TAG_TEXT = 0x0030,
    TAG_TIMESTAMP = 0x0031,
    TAG_PEER_NICK = 0x0032,
    TAG_PEER_UID = 0x0033,
    TAG_SCOPE = 0x0034,
    TAG_ERROR_CODE = 0x0040,
    TAG_ERROR_MSG = 0x0041
} tlv_tag_t;

typedef enum {
    ERR_UNKNOWN = 1,
    ERR_PROTOCOL = 2,
    ERR_BAD_FRAME = 3,
    ERR_AUTH_REQUIRED = 4,
    ERR_NICK_INVALID = 5,
    ERR_NICK_TAKEN = 6,
    ERR_NO_SUCH_ROOM = 7,
    ERR_NOT_IN_ROOM = 8,
    ERR_RATE_LIMITED = 9,
    ERR_SLOW_CLIENT = 10,
    ERR_PERMISSION_DENIED = 11,
    ERR_USER_EXISTS = 12,
    ERR_AUTH_FAILED = 13,
    ERR_NOT_LOGGED_IN = 14
} error_code_t;
