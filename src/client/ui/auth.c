#include "client/forms.h"
#include "client/text.h"
#include "client/ui.h"
#include <ncurses.h>
#include <string.h>

void ui_auth_clear_secrets(ui_state_t *ui) {
    ui->auth_deferred = false;
    explicit_bzero(ui->auth_password, sizeof(ui->auth_password));
    explicit_bzero(ui->auth_confirm, sizeof(ui->auth_confirm));
    explicit_bzero(ui->auth_server_password, sizeof(ui->auth_server_password));
    ui->auth_cursors[1] = ui->auth_cursors[2] = ui->auth_cursors[3] = 0;
}

static int field_count(const ui_state_t *ui) {
    return 2 + ui->auth_register + ui->auth_server_required;
}

static void toggle_mode(ui_state_t *ui) {
    ui->auth_register = !ui->auth_register;
    ui->auth_focus = 0;
    ui->auth_error[0] = '\0';
    ui_auth_clear_secrets(ui);
}

static void geometry(ui_state_t *ui, int *height, int *width, int *top, int *left, int *first) {
    *height = ui->layout.rows - 2;
    if (*height > 12)
        *height = 12;
    *width = ui->layout.cols - 2;
    if (*width > 64)
        *width = 64;
    *top = (ui->layout.rows - *height) / 2;
    *left = (ui->layout.cols - *width) / 2;
    int fields = field_count(ui);
    *first = fields <= *height - 5 ? 0 : ui->auth_focus < fields ? ui->auth_focus : fields - 1;
}

static char *field(ui_state_t *ui, int index) {
    if (index == 0)
        return ui->auth_name;
    if (index == 1)
        return ui->auth_password;
    return ui->auth_register && index == 2 ? ui->auth_confirm : ui->auth_server_password;
}

int ui_auth_input(ui_state_t *ui, int key, uint32_t character, bool printable) {
    ui->modal_dirty = true;
    if (key == 0x11 || key == KEY_F(10))
        return UI_QUIT;
    if (ui->auth_pending)
        return UI_NONE;
    if (key == 27) {
        if (!ui->auth_register)
            return UI_QUIT;
        toggle_mode(ui);
        return UI_NONE;
    }
    int fields = field_count(ui);
    if (key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) != OK || !(mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
            return UI_NONE;
        int h, w, y, x, first;
        geometry(ui, &h, &w, &y, &x, &first);
        int row = mouse.y - y, column = mouse.x - x;
        if (column < 1 || column >= w - 1)
            return UI_NONE;
        if (row >= 2 && row < h - 3 && first + row - 2 < fields) {
            ui->auth_focus = first + row - 2;
            ui->auth_cursors[ui->auth_focus] = (int)strlen(field(ui, ui->auth_focus));
        } else if (row == h - 2) {
            const char *buttons[] = {ui->auth_register ? "[Create]" : "[Log in]",
                                     ui->auth_register ? "[Back]" : "[Create account]", "[Quit]"};
            int start = 2;
            for (int i = 0; i < 3; i++) {
                if (column >= start && column < start + (int)strlen(buttons[i])) {
                    ui->auth_focus = fields + i;
                    key = '\n';
                    break;
                }
                start += (int)strlen(buttons[i]) + 1;
            }
        }
    }
    if (key == '\t' || key == KEY_DOWN)
        ui->auth_focus = (ui->auth_focus + 1) % (fields + 3);
    else if (key == KEY_BTAB || key == KEY_UP)
        ui->auth_focus = (ui->auth_focus + fields + 2) % (fields + 3);
    else if (key == '\n' || key == KEY_ENTER) {
        if (ui->auth_focus < fields - 1)
            ui->auth_focus++;
        else if (ui->auth_focus <= fields)
            return UI_AUTH_SUBMIT;
        else if (ui->auth_focus == fields + 1)
            toggle_mode(ui);
        else
            return UI_QUIT;
    } else if (ui->auth_focus < fields) {
        if (ui->auth_focus == 0 && printable && character >= 32 &&
            !(character == '_' || character == '-' || (character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')))
            return UI_NONE;
        form_edit(field(ui, ui->auth_focus),
                  ui->auth_focus == 0 ? sizeof(ui->auth_name) : sizeof(ui->auth_password),
                  &ui->auth_cursors[ui->auth_focus], key, character, printable);
    }
    return UI_NONE;
}

void ui_redraw_auth(ui_state_t *ui, const cli_model_t *m) {
    int h, w, y, x, first;
    geometry(ui, &h, &w, &y, &x, &first);
    WINDOW *window = ui_modal_window(ui, UI_MODAL_AUTH, h, w, y, x);
    if (!window)
        return;
    box(window, 0, 0);
    wattron(window, A_BOLD | COLOR_PAIR(1));
    mvwaddstr(window, 0, 2,
              ui->auth_register ? " Create a CupidChat account " : " CupidChat sign in ");
    wattroff(window, A_BOLD | COLOR_PAIR(1));
    mvwaddnstr(window, 1, 2, "An account is required to chat.", w - 4);
    const char *labels[] = {"Account", "Password", ui->auth_register ? "Confirm" : "Server key",
                            "Server key"};
    int fields = field_count(ui);
    int cursor_row = -1, cursor_column = 0;
    for (int i = first, row = 2; i < fields && row < h - 3; i++, row++) {
        mvwaddnstr(window, row, 2, labels[i], 10);
        char masked[256];
        const char *value = field(ui, i);
        int position = ui->auth_cursors[i];
        if (i) {
            int count = form_char_count(value, (int)strlen(value));
            memset(masked, '*', (size_t)count);
            masked[count] = '\0';
            position = form_char_count(value, position);
            value = masked;
        }
        int visible = w - 16;
        int start = position >= visible ? position - visible + 1 : 0;
        if (ui->auth_focus == i)
            wattron(window, A_REVERSE);
        mvwhline(window, row, 14, ' ', visible);
        mvwaddnstr(window, row, 14, value + start, visible);
        wattroff(window, A_REVERSE);
        if (ui->auth_focus == i) {
            cursor_row = row;
            cursor_column = 14 + position - start;
        }
    }
    const char *message = ui->auth_pending              ? "Signing in..."
                          : ui->auth_error[0]           ? ui->auth_error
                          : m->status == CLI_CONNECTING ? "Waiting for server..."
                                                        : "Tab changes fields; Enter continues";
    mvwaddnstr(window, h - 3, 2, message, w - 4);
    const char *buttons[] = {ui->auth_register ? "[Create]" : "[Log in]",
                             ui->auth_register ? "[Back]" : "[Create account]", "[Quit]"};
    int column = 2;
    for (int i = 0; i < 3; i++) {
        if (ui->auth_focus == fields + i)
            wattron(window, A_REVERSE);
        mvwaddnstr(window, h - 2, column, buttons[i], w - column - 1);
        wattroff(window, A_REVERSE);
        column += (int)strlen(buttons[i]) + 1;
    }
    if (cursor_row >= 0)
        wmove(window, cursor_row, cursor_column);
    wnoutrefresh(window);
}
