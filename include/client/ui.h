/* CupidChat public interface. */
#pragma once
#include "client/board.h"
#include "client/model.h"
#include "client/room_tools.h"

typedef struct {
    int rows, cols, rooms_w, users_w, chat_w, chat_h;
    bool too_small;
} ui_layout_t;

typedef enum {
    UI_MODAL_AUTH,
    UI_MODAL_HELP,
    UI_MODAL_EMOJI,
    UI_MODAL_BUDDIES,
    UI_MODAL_BOARD,
    UI_MODAL_ROOM,
    UI_MODAL_PROFILE,
    UI_MODAL_POLL,
    UI_MODAL_QUIT,
    UI_MODAL_COUNT
} ui_modal_t;

enum {
    UI_NONE,
    UI_LINE,
    UI_PAGE_UP,
    UI_PAGE_DOWN,
    UI_SELECT,
    UI_RESIZE,
    UI_SCROLL,
    UI_CLOSE,
    UI_LATEST,
    UI_NEXT_UNREAD,
    UI_BUDDY_SELECT,
    UI_BUDDY_OPEN,
    UI_AUTH_SUBMIT,
    UI_PROFILE_OPEN,
    UI_PROFILE_SAVE,
    UI_PROFILE_MESSAGE,
    UI_PROFILE_BUDDY,
    UI_PROFILE_REFRESH,
    UI_PROFILE_INVITE,
    UI_POLL_OPEN,
    UI_POLL_VOTE,
    UI_BOARD_OPEN,
    UI_BOARD_THREAD,
    UI_BOARD_LIST,
    UI_BOARD_PREV,
    UI_BOARD_NEXT,
    UI_BOARD_POST,
    UI_BOARD_DELETE,
    UI_BOARD_PIN,
    UI_BOARD_LOCK,
    UI_ROOM_OPEN,
    UI_ROOM_RUN,
    UI_ROOM_REFRESH,
    UI_QUIT
};

typedef struct {
    ui_layout_t layout;
    board_state_t board;
    room_control_state_t room_control;
    void *win_topbar, *win_topic, *win_rooms, *win_chat, *win_users, *win_input;
    void *modal_windows[UI_MODAL_COUNT];
    unsigned painted_modals;
    bool show_users, dirty, show_help_modal, show_emoji_modal, show_buddies_modal, confirm_quit;
    bool input_dirty, modal_dirty;
    bool show_poll, poll_pending;
    int poll_cursor, poll_question_row;
    bool poll_requested;
    int buddy_cursor;
    int emoji_cursor;
    bool show_auth, auth_register, auth_pending, auth_deferred;
    bool auth_server_required;
    int auth_focus, auth_cursors[4];
    char auth_name[33], auth_password[256], auth_confirm[256], auth_error[192];
    char auth_server_password[256];
    bool show_profile, profile_loading, profile_editing, profile_saving, profile_discard;
    profile_info_t profile;
    profile_fields_t profile_draft;
    uint32_t profile_request, profile_target_uid;
    char profile_target_nick[33], profile_target_account[33], profile_error[192];
    int profile_focus, profile_offset, profile_cursors[PROFILE_FIELD_COUNT];
    int profile_buttons[6][4], profile_button_count, users_offset;

    struct {
        int row, left, right;
        uint32_t uid;
        char nick[33];
    } name_hits[CLI_HISTORY_CAP];

    int name_hit_count;
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
void *ui_modal_window(ui_state_t *ui, ui_modal_t modal, int height, int width, int y, int x);
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
void ui_auth_clear_secrets(ui_state_t *ui);
int ui_auth_input(ui_state_t *ui, int key, uint32_t character, bool printable);
void ui_redraw_auth(ui_state_t *ui, const cli_model_t *m);
int ui_profile_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                     bool printable);
void ui_redraw_profile(ui_state_t *ui, const cli_model_t *m);
int ui_profile_target(ui_state_t *ui, uint32_t uid, const char *nick, const char *account);

int ui_poll_input(ui_state_t *ui, const cli_model_t *m, int key);
void ui_redraw_poll(ui_state_t *ui, const cli_model_t *m);

int ui_board_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                   bool printable);
void ui_redraw_board(ui_state_t *ui, const cli_model_t *m);

int ui_room_control_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                          bool printable);
void ui_redraw_room_control(ui_state_t *ui, const cli_model_t *m);
