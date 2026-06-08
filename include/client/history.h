#ifndef CLIENT_HISTORY_H
#define CLIENT_HISTORY_H

#include "client/model.h"

typedef struct {
    const cli_msg_t *buf;
    int cap;
    int total;
    int oldest_idx;
    int pos;
    int end;
} history_iter_t;

void history_iter_init(history_iter_t *it, const cli_msg_t *buf, int cap,
                       int head, int len, int scroll, int visible_lines);

const cli_msg_t *history_iter_next(history_iter_t *it);

#endif
