/* CupidChat public interface. */
#pragma once
#include "client/model.h"

typedef struct {
    const cli_msg_t *buf;
    int cap, total, oldest_idx, pos, end;
} history_iter_t;

void history_iter_init(history_iter_t *it, const cli_msg_t *buf, int cap, int head, int len,
                       int scroll, int visible_lines);
const cli_msg_t *history_iter_next(history_iter_t *it);
bool history_same_day(uint64_t a, uint64_t b);
int history_prefix_width(const cli_msg_t *msg, int width);
int history_message_rows(cli_msg_t *msg, int width, bool ignored);
int history_total_rows(cli_msg_t *buf, int cap, int head, int len, int width, const cli_model_t *m);
int history_reflow_scroll(cli_msg_t *buf, int cap, int head, int len, int old_width, int new_width,
                          int scroll, const cli_model_t *m);
