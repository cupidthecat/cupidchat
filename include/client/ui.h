/* CupidChat public interface. */
#pragma once
#include "client/model.h"
typedef struct {
    int rows, cols, rooms_w, users_w, chat_w, chat_h;
    bool too_small;
} ui_layout_t;

enum {
    UI_NONE, UI_LINE, UI_PAGE_UP, UI_PAGE_DOWN, UI_SELECT, UI_RESIZE,
    UI_SCROLL, UI_CLOSE, UI_LATEST, UI_QUIT
};
typedef struct {
    ui_layout_t layout;
    void *win_topbar, *win_topic, *win_rooms, *win_chat, *win_users, *win_input;
    bool show_users, dirty, show_help_modal, confirm_quit;
    char input_buf[4096];
    int input_len, input_cursor, input_offset, nav_cursor;
    int sidebar_offset, help_offset, scroll_delta;
    char *hist[64];
    int hist_count, hist_idx;
} ui_state_t;

int ui_init(ui_state_t *ui);
void ui_compute_layout(ui_layout_t *l, int rows, int cols, bool show_users);
void ui_free(ui_state_t *ui);
void ui_resize(ui_state_t *ui);
int ui_input_tick(ui_state_t *ui, const cli_model_t *m, char *out_line, int out_max);
int ui_click(ui_state_t *ui, const cli_model_t *m, int x, int y);
void ui_clamp_navigation(ui_state_t *ui, const cli_model_t *m);
void ui_scroll(ui_state_t *ui, cli_model_t *m, int delta);
void ui_set_status(ui_state_t *ui, const char *msg);
void ui_redraw_topic(ui_state_t *ui, const cli_model_t *m);
void ui_redraw_topbar(ui_state_t *ui, const cli_model_t *m);
void ui_redraw_rooms(ui_state_t *ui, const cli_model_t *m);
void ui_redraw_chat(ui_state_t *ui, cli_model_t *m);
void ui_redraw_users(ui_state_t *ui, const cli_model_t *m);
void ui_redraw_input(ui_state_t *ui);
void ui_redraw(ui_state_t *ui, cli_model_t *m);
