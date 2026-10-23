#pragma once
#include "proto/room_tools.h"

typedef struct {
    bool visible, pending, loaded, from_profile;
    uint32_t request;
    room_info_t info;
    char target[33], reason[257], message[192];
    int focus, action, cursors[2], scroll;
} room_control_state_t;
