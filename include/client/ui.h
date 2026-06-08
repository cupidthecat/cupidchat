#ifndef CLIENT_UI_H
#define CLIENT_UI_H

#include <stdbool.h>
#include "client/model.h"

#define K_RET_NONE 0
#define K_RET_LINE 1
#define K_RET_QUIT 2

typedef struct {
    int rows;
    int cols;
    int rooms_w;
    int users_w;
    int chat_w;
    int chat_h;
} ui_layout_t;

typedef struct ui_state_t ui_state_t;
struct ui_state_t {
    ui_layout_t layout;
    
    void *win_topbar;
    void *win_topic;
    void *win_rooms;
    void *win_chat;
    void *win_users;
    void *win_input;
    
    char input_buf[1024];
    int input_len;
    int input_cursor;
    
    bool dirty;
    bool show_users;
    bool show_help_modal;
    int nav_cursor;
    
    char *hist[64];
    int hist_count;
    int hist_idx;
};

int ui_init(ui_state_t *ui);
void ui_free(ui_state_t *ui);
void ui_resize(ui_state_t *ui);

int ui_input_tick(ui_state_t *ui, char *out_line, int out_max);
void ui_scroll(ui_state_t *ui, cli_model_t *m, int delta);
void ui_set_status(ui_state_t *ui, const char *msg);
void ui_redraw(ui_state_t *ui, const cli_model_t *m);

#endif
