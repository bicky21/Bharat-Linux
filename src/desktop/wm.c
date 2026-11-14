/*
 * wm.c - the window manager.
 *
 * Keeps the list of open windows (bottom to top), draws the whole
 * desktop picture, and decides who gets each mouse click and key press.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "desktop.h"

#define MAX_WINDOWS 16
#define GRIP 14              /* size of the resize corner */
#define BUTTON_W 40          /* width of a title-bar button */

static struct window *wins[MAX_WINDOWS];   /* [0] = bottom, last = top */
static int nwin;

static int need_redraw = 1;
int wm_quit;

/* what the mouse is doing right now */
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

static int drag_mode;
static struct window *drag_win;
static int drag_dx, drag_dy;

static int menu_open;
static int selected_icon = -1;

/* for double clicks */
static long long last_click_ms;
static int last_click_x, last_click_y;

/* ---------- small helpers ---------- */

long long now_ms(void) {

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int inside(int px, int py, int x, int y, int w, int h) {

    return px >= x && py >= y && px < x + w && py < y + h;
}

void wm_redraw(void) {

    need_redraw = 1;
}

int wm_needs_redraw(void) {

    return need_redraw;
}

void content_box(struct window *win, int *x, int *y, int *w, int *h) {

    *x = win->x + 1;
    *y = win->y + TITLE_H;
    *w = win->w - 2;
    *h = win->h - TITLE_H - 1;
}

/* Cut a text down so it fits in max_w pixels, adding "..." if needed. */
static void fit_text(struct font *f, const char *s, int max_w, char *out, size_t size) {

    snprintf(out, size, "%s", s);

    if (text_width(f, out) <= max_w)
        return;

    size_t len = strlen(out);

    while (len > 0) {

        out[--len] = 0;

        char tmp[256];

        snprintf(tmp, sizeof(tmp), "%s...", out);

        if (text_width(f, tmp) <= max_w) {

            snprintf(out, size, "%.*s", (int)size - 1, tmp);
            return;
        }
    }
}

static struct window *top_window(void) {

    for (int i = nwin - 1; i >= 0; i--) {

        if (!wins[i]->minimized)
            return wins[i];
    }

    return NULL;
}

static void raise_window(struct window *win) {

    int at = -1;

    for (int i = 0; i < nwin; i++) {

        if (wins[i] == win)
            at = i;
    }

    if (at < 0)
        return;

    memmove(&wins[at], &wins[at + 1], sizeof(wins[0]) * (nwin - at - 1));
    wins[nwin - 1] = win;
    win->minimized = 0;

    need_redraw = 1;
}

/* ---------- creating and closing windows ---------- */

struct window *wm_new_window(const char *title, int w, int h) {

    if (nwin >= MAX_WINDOWS)
        return NULL;

    struct window *win = calloc(1, sizeof(*win));

    if (!win)
        return NULL;

    static int cascade;

    if (w > screen_w - 40) w = screen_w - 40;
    if (h > screen_h - TASKBAR_H - 40) h = screen_h - TASKBAR_H - 40;

    win->w = w;
    win->h = h;
    win->min_w = 220;
    win->min_h = 140;
    win->x = 150 + (cascade % 8) * 30;
    win->y = 30 + (cascade % 8) * 30;

    if (win->x + w > screen_w) win->x = screen_w - w - 10;
    if (win->y + h > screen_h - TASKBAR_H) win->y = 10;

    cascade++;

    snprintf(win->title, sizeof(win->title), "%s", title);

    wins[nwin++] = win;
    need_redraw = 1;

    return win;
}

static void destroy(struct window *win) {

    if (win->destroy)
        win->destroy(win);

    if (drag_win == win) {

        drag_win = NULL;
        drag_mode = DRAG_NONE;
    }

    free(win);
}

void wm_cleanup(void) {

    int out = 0;

    for (int i = 0; i < nwin; i++) {

        if (wins[i]->want_close) {

            destroy(wins[i]);
            need_redraw = 1;
        }
        else {

            wins[out++] = wins[i];
        }
    }

    nwin = out;
}

void wm_close_all(void) {

    for (int i = 0; i < nwin; i++)
        destroy(wins[i]);

    nwin = 0;
}

int wm_poll_fds(int *fds, struct window **owners, int max) {

    int n = 0;

    for (int i = 0; i < nwin && n < max; i++) {

        if (!wins[i]->fd)
            continue;

        int fd = wins[i]->fd(wins[i]);

        if (fd >= 0) {

            fds[n] = fd;
            owners[n] = wins[i];
            n++;
        }
    }

    return n;
}

void wm_tick(void) {

    for (int i = 0; i < nwin; i++) {

        if (wins[i]->tick)
            wins[i]->tick(wins[i]);
    }

    /* the clock in the taskbar */
    need_redraw = 1;
}

/* ---------- icons, drawn with shapes ---------- */

void icon_terminal(int x, int y, int s) {

    fill_round(x, y + s / 8, s, s * 3 / 4, s / 8, 0x24292F);
    fill(x + 2, y + s / 8 + s / 8, s - 4, 1, 0x3D444D);

    /* a little ">_" prompt */
    int cx = x + s / 5, cy = y + s / 2;

    for (int i = 0; i < s / 6; i++) {

        put_pixel(cx + i, cy - s / 6 + i, C_GREEN, 255);
        put_pixel(cx + i, cy - s / 6 + i + 1, C_GREEN, 255);
        put_pixel(cx + i, cy + s / 6 - i, C_GREEN, 255);
        put_pixel(cx + i, cy + s / 6 - i - 1, C_GREEN, 255);
    }

    fill(cx + s / 4, cy + s / 6 - 1, s / 4, 2, C_WHITE);
}

void icon_folder(int x, int y, int s) {

    fill_round(x, y + s / 6, s * 2 / 5, s / 4, s / 12, C_SAFFRON_D);
    fill_round(x, y + s / 4, s, s * 2 / 3, s / 10, C_SAFFRON_D);
    fill_round(x, y + s / 3, s, s * 7 / 12, s / 10, C_SAFFRON);
}

void icon_file(int x, int y, int s) {

    int w = s * 3 / 4, ox = x + (s - w) / 2;

    fill(ox, y, w, s, 0x8C959F);
    fill(ox + 1, y + 1, w - 2, s - 2, C_WHITE);

    /* folded corner */
    fill(ox + w - s / 4, y, s / 4, s / 4, 0xD8DEE4);

    for (int i = 1; i < 4; i++)
        fill(ox + s / 8, y + i * s / 4 + 2, w - s / 4, 1, 0xAFB8C1);
}

void icon_monitor(int x, int y, int s) {

    fill_round(x, y + s / 8, s, s * 3 / 4, s / 8, 0x0D3B2E);

    int bx = x + s / 8, by = y + s * 3 / 4, bw = s * 3 / 4;
    int px = bx, py = by;
    /* pairs of (height %, position %) along the graph */
    int points[] = {20, 0, 55, 25, 35, 45, 75, 65, 55, 85, 90, 100};

    for (int i = 0; i < 12; i += 2) {

        int nx = bx + points[i + 1] * bw / 100;
        int ny = by - points[i] * s / 2 / 100 - 2;

        if (i > 0) {

            line(px, py, nx, ny, 0x56D364);
            line(px, py + 1, nx, ny + 1, 0x56D364);
        }

        px = nx;
        py = ny;
    }
}

void icon_about(int x, int y, int s) {

    fill_circle(x + s / 2, y + s / 2, s / 2 - 1, C_NAVY);
    fill_circle(x + s / 2, y + s / 3, s / 14 + 1, C_WHITE);
    fill(x + s / 2 - s / 14, y + s / 2 - 2, s / 7, s / 3, C_WHITE);
}

/* ---------- desktop icons and the start menu entries ---------- */

struct launcher {
    const char *name;
    const char *short_name;      /* under the desktop icon */
    void (*icon)(int x, int y, int s);
    void (*open)(void);
};

static void open_home_files(void) {

    char *home = getenv("HOME");

    open_files(home ? home : "/");
}

static struct launcher launchers[] = {
    {"Terminal",       "Terminal",     icon_terminal, open_terminal},
    {"Files",          "Files",        icon_folder,   open_home_files},
    {"System Monitor", "Monitor",      icon_monitor,  open_monitor},
    {"About Bharat",   "About Bharat", icon_about,    open_about},
};

#define LAUNCHERS ((int)(sizeof(launchers) / sizeof(launchers[0])))

#define ICON_CELL_W 104
#define ICON_CELL_H 92

static void icon_cell(int i, int *x, int *y) {

    *x = 16;
    *y = 16 + i * ICON_CELL_H;
}

static void draw_desktop_icons(void) {

    for (int i = 0; i < LAUNCHERS; i++) {

        int x, y;

        icon_cell(i, &x, &y);

        if (i == selected_icon)
            fill_round(x, y, ICON_CELL_W, ICON_CELL_H - 6, 8, 0x5A7FB0);

        launchers[i].icon(x + (ICON_CELL_W - 44) / 2, y + 8, 44);

        char label[64];

        fit_text(&font_ui, launchers[i].short_name, ICON_CELL_W - 4, label, sizeof(label));

        int tw = text_width(&font_ui, label);
        int tx = x + (ICON_CELL_W - tw) / 2;

        /* a soft shadow makes white text readable on any background */
        text_alpha(&font_ui, tx + 1, y + 61, label, 0x000000, 160);
        text(&font_ui, tx, y + 60, label, C_WHITE);
    }
}

static int icon_at(int px, int py) {

    for (int i = 0; i < LAUNCHERS; i++) {

        int x, y;

        icon_cell(i, &x, &y);

        if (inside(px, py, x, y, ICON_CELL_W, ICON_CELL_H - 6))
            return i;
    }

    return -1;
}

/* ---------- start menu ---------- */

#define MENU_W 270
#define MENU_HEAD 70
#define MENU_ITEM 38

static int menu_h(void) {

    return MENU_HEAD + (LAUNCHERS + 1) * MENU_ITEM + 18;
}

static void menu_box(int *x, int *y) {

    *x = 6;
    *y = screen_h - TASKBAR_H - menu_h() - 6;
}

static void soft_shadow(int x, int y, int w, int h) {

    fill_alpha(x + 3, y + 5, w, h, 0x000000, 40);
    fill_alpha(x + 1, y + 2, w + 2, h + 2, 0x000000, 30);
}

static void draw_menu(void) {

    int x, y;

    menu_box(&x, &y);

    int h = menu_h();

    soft_shadow(x, y, MENU_W, h);
    fill_round(x, y, MENU_W, h, 10, C_WHITE);

    /* header: logo, name, user */
    draw_logo(x + 16, y + 14, 42);
    text(&font_bold, x + 70, y + 18, "Bharat-linux", C_TEXT);

    char who[96];
    char *user = getenv("USER");

    snprintf(who, sizeof(who), "Signed in as %s", user ? user : "?");
    text(&font_ui, x + 70, y + 37, who, C_MUTED);

    fill(x + 12, y + MENU_HEAD - 1, MENU_W - 24, 1, C_BORDER);

    for (int i = 0; i <= LAUNCHERS; i++) {

        int iy = y + MENU_HEAD + 4 + i * MENU_ITEM;

        if (i == LAUNCHERS) {

            iy += 8;
            fill(x + 12, iy - 6, MENU_W - 24, 1, C_BORDER);
        }

        if (inside(mouse_x, mouse_y, x + 6, iy, MENU_W - 12, MENU_ITEM))
            fill_round(x + 6, iy, MENU_W - 12, MENU_ITEM, 6, C_SELECT);

        if (i < LAUNCHERS) {

            launchers[i].icon(x + 16, iy + 7, 24);
            text(&font_ui, x + 52, iy + 10, launchers[i].name, C_TEXT);
        }
        else {

            /* "power" symbol: a ring with a bar */
            fill_circle(x + 28, iy + 19, 10, 0xCF222E);
            fill_circle(x + 28, iy + 19, 7, C_WHITE);
            fill(x + 26, iy + 6, 5, 10, C_WHITE);
            fill(x + 27, iy + 8, 3, 10, 0xCF222E);
            text(&font_ui, x + 52, iy + 10, "Exit desktop", C_TEXT);
        }
    }
}

/* Returns the item number under the mouse, LAUNCHERS for "Exit", -1 none. */
static int menu_item_at(int px, int py) {

    int x, y;

    menu_box(&x, &y);

    for (int i = 0; i <= LAUNCHERS; i++) {

        int iy = y + MENU_HEAD + 4 + i * MENU_ITEM + (i == LAUNCHERS ? 8 : 0);

        if (inside(px, py, x + 6, iy, MENU_W - 12, MENU_ITEM))
            return i;
    }

    return -1;
}

static int in_menu(int px, int py) {

    int x, y;

    menu_box(&x, &y);

    return inside(px, py, x, y, MENU_W, menu_h());
}

/* ---------- taskbar ---------- */

#define START_W 112

static int task_button_w(void) {

    int space = screen_w - START_W - 150;
    int w = nwin ? space / nwin : 180;

    return w > 180 ? 180 : w;
}

static void draw_taskbar(void) {

    int y = screen_h - TASKBAR_H;

    fill_alpha(0, y, screen_w, TASKBAR_H, 0x0D1117, 225);
    fill(0, y, screen_w, 2, C_SAFFRON);

    /* start button */
    if (menu_open || inside(mouse_x, mouse_y, 4, y + 4, START_W - 8, TASKBAR_H - 6))
        fill_round(4, y + 5, START_W - 8, TASKBAR_H - 8, 6, 0x30363D);

    draw_logo(12, y + 9, 26);
    text(&font_bold, 46, y + 13, "Bharat", C_WHITE);

    /* one button per window */
    int bw = task_button_w();
    struct window *top = top_window();

    for (int i = 0; i < nwin; i++) {

        int bx = START_W + i * bw;
        int active = wins[i] == top;

        fill_round(bx + 2, y + 6, bw - 4, TASKBAR_H - 10, 5,
                   active ? 0x3A4558 : 0x21262D);

        if (active)
            fill(bx + 10, y + TASKBAR_H - 6, bw - 20, 2, C_SAFFRON);

        char label[64];

        fit_text(&font_ui, wins[i]->title, bw - 20, label, sizeof(label));
        text(&font_ui, bx + 10, y + 12, label,
             wins[i]->minimized ? 0x8B949E : C_WHITE);
    }

    /* clock and date */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char clock_text[16], date_text[32];

    strftime(clock_text, sizeof(clock_text), "%H:%M", tm);
    strftime(date_text, sizeof(date_text), "%a %d %b", tm);

    int cw = text_width(&font_bold, clock_text);
    int dw = text_width(&font_ui, date_text);

    text(&font_bold, screen_w - 14 - cw, y + 4, clock_text, C_WHITE);
    text(&font_ui, screen_w - 14 - dw, y + 21, date_text, 0xC9D1D9);
}

/* ---------- windows ---------- */

static void draw_window(struct window *win, int active) {

    int x = win->x, y = win->y, w = win->w, h = win->h;

    soft_shadow(x, y, w, h);

    /* title bar */
    if (active)
        gradient(x, y, w, TITLE_H, 0x2B4170, C_NAVY);
    else
        fill(x, y, w, TITLE_H, 0xE6E9ED);

    fill(x, y + TITLE_H - 1, w, 1, active ? C_SAFFRON : C_BORDER);

    uint32_t title_color = active ? C_WHITE : 0x57606A;

    char title[64];

    fit_text(&font_bold, win->title, w - 2 * BUTTON_W - 20, title, sizeof(title));
    text(&font_bold, x + 12, y + 7, title, title_color);

    /* close button (x) */
    int cx = x + w - BUTTON_W;

    if (inside(mouse_x, mouse_y, cx, y, BUTTON_W, TITLE_H - 1)) {

        fill(cx, y, BUTTON_W, TITLE_H - 1, 0xCF222E);
        title_color = C_WHITE;
    }

    for (int i = -4; i <= 4; i++) {

        put_pixel(cx + 20 + i, y + 14 + i, title_color, 255);
        put_pixel(cx + 21 + i, y + 14 + i, title_color, 255);
        put_pixel(cx + 20 + i, y + 14 - i, title_color, 255);
        put_pixel(cx + 21 + i, y + 14 - i, title_color, 255);
    }

    /* minimise button (_) */
    int mx = cx - BUTTON_W;

    title_color = active ? C_WHITE : 0x57606A;

    if (inside(mouse_x, mouse_y, mx, y, BUTTON_W, TITLE_H - 1))
        fill(mx, y, BUTTON_W, TITLE_H - 1, active ? 0x3B5488 : 0xD0D7DE);

    fill(mx + 15, y + 15, 11, 2, title_color);

    /* the body: the app draws itself, clipped to its own box */
    int bx, by, bw, bh;

    content_box(win, &bx, &by, &bw, &bh);

    fill(bx, by, bw, bh, C_WHITE);

    if (win->draw) {

        gfx_clip(bx, by, bw, bh);
        win->draw(win, bx, by, bw, bh);
        gfx_noclip();
    }

    frame(x, y, w, h, active ? 0x1B2A4A : 0x8C959F);

    /* resize grip: three little dots */
    for (int i = 0; i < 3; i++)
        fill(x + w - 5 - i * 4, y + h - 5, 2, 2, 0x8C959F);

    fill(x + w - 5, y + h - 9, 2, 2, 0x8C959F);
}

void wm_draw(void) {

    restore_background();

    draw_desktop_icons();

    struct window *top = top_window();

    for (int i = 0; i < nwin; i++) {

        if (!wins[i]->minimized)
            draw_window(wins[i], wins[i] == top);
    }

    draw_taskbar();

    if (menu_open)
        draw_menu();

    draw_cursor(mouse_x, mouse_y);

    gfx_flush();

    need_redraw = 0;
}

/* ---------- mouse and keyboard ---------- */

static void launch(int i) {

    menu_open = 0;

    if (i >= 0 && i < LAUNCHERS)
        launchers[i].open();
    else if (i == LAUNCHERS)
        wm_quit = 1;

    need_redraw = 1;
}

static struct window *window_at(int px, int py) {

    for (int i = nwin - 1; i >= 0; i--) {

        struct window *w = wins[i];

        if (!w->minimized && inside(px, py, w->x, w->y, w->w, w->h))
            return w;
    }

    return NULL;
}

static void keep_on_screen(struct window *win) {

    if (win->x > screen_w - 60) win->x = screen_w - 60;
    if (win->x + win->w < 60) win->x = 60 - win->w;
    if (win->y < 0) win->y = 0;
    if (win->y > screen_h - TASKBAR_H - TITLE_H) win->y = screen_h - TASKBAR_H - TITLE_H;
}

static void mouse_down(struct event *ev) {

    int px = mouse_x, py = mouse_y;

    long long t = now_ms();
    int dbl = t - last_click_ms < 450 &&
              abs(px - last_click_x) < 5 &&
              abs(py - last_click_y) < 5;

    last_click_ms = dbl ? 0 : t;   /* a third click starts over */
    last_click_x = px;
    last_click_y = py;

    need_redraw = 1;

    /* 1. the start menu is open: it gets the click */
    if (menu_open) {

        if (in_menu(px, py)) {

            launch(menu_item_at(px, py));
            return;
        }

        menu_open = 0;

        /* clicking the start button again just closes the menu */
        if (py >= screen_h - TASKBAR_H && px < START_W)
            return;
    }

    /* 2. the taskbar */
    if (py >= screen_h - TASKBAR_H) {

        if (px < START_W) {

            menu_open = 1;
            return;
        }

        int i = (px - START_W) / task_button_w();

        if (px >= START_W && i >= 0 && i < nwin) {

            struct window *win = wins[i];

            if (win == top_window())
                win->minimized = 1;     /* click on the active one hides it */
            else
                raise_window(win);
        }

        return;
    }

    /* 3. a window */
    struct window *win = window_at(px, py);

    if (win) {

        raise_window(win);

        int rx = px - win->x, ry = py - win->y;

        if (ry < TITLE_H) {

            if (ev->button != 1)
                return;

            if (rx >= win->w - BUTTON_W) {

                win->want_close = 1;
            }
            else if (rx >= win->w - 2 * BUTTON_W) {

                win->minimized = 1;
            }
            else {

                drag_mode = DRAG_MOVE;
                drag_win = win;
                drag_dx = rx;
                drag_dy = ry;
            }

            return;
        }

        if (ev->button == 1 && rx >= win->w - GRIP && ry >= win->h - GRIP) {

            drag_mode = DRAG_RESIZE;
            drag_win = win;
            drag_dx = win->w - rx;
            drag_dy = win->h - ry;

            return;
        }

        int bx, by, bw, bh;

        content_box(win, &bx, &by, &bw, &bh);

        if (win->click && inside(px, py, bx, by, bw, bh))
            win->click(win, px - bx, py - by, ev->button, dbl);

        return;
    }

    /* 4. the desktop itself */
    int icon = icon_at(px, py);

    selected_icon = icon;

    if (icon >= 0 && dbl && ev->button == 1)
        launch(icon);
}

static void mouse_move(void) {

    if (drag_mode == DRAG_MOVE && drag_win) {

        drag_win->x = mouse_x - drag_dx;
        drag_win->y = mouse_y - drag_dy;

        keep_on_screen(drag_win);
    }
    else if (drag_mode == DRAG_RESIZE && drag_win) {

        int w = mouse_x - drag_win->x + drag_dx;
        int h = mouse_y - drag_win->y + drag_dy;

        if (w < drag_win->min_w) w = drag_win->min_w;
        if (h < drag_win->min_h) h = drag_win->min_h;

        if (w != drag_win->w || h != drag_win->h) {

            drag_win->w = w;
            drag_win->h = h;

            if (drag_win->resized)
                drag_win->resized(drag_win);
        }
    }

    need_redraw = 1;
}

static void key_press(struct event *ev) {

    /* Ctrl+Alt+Backspace: leave the desktop */
    if ((ev->mods & MOD_CTRL) && (ev->mods & MOD_ALT) && ev->key == '\b') {

        wm_quit = 1;
        return;
    }

    /* Ctrl+Alt+T: new terminal */
    if ((ev->mods & MOD_CTRL) && (ev->mods & MOD_ALT) && (ev->key == 't' || ev->key == 'T')) {

        open_terminal();
        return;
    }

    struct window *top = top_window();

    /* Alt+Tab: bring the bottom window to the top */
    if ((ev->mods & MOD_ALT) && ev->key == '\t') {

        if (nwin > 0)
            raise_window(wins[0]);

        return;
    }

    /* Alt+F4: close the active window */
    if ((ev->mods & MOD_ALT) && ev->key == K_F4) {

        if (top)
            top->want_close = 1;

        need_redraw = 1;
        return;
    }

    if (ev->key == 27 && menu_open) {

        menu_open = 0;
        need_redraw = 1;
        return;
    }

    if (top && top->key) {

        top->key(top, ev);
        need_redraw = 1;
    }
}

void wm_event(struct event *ev) {

    switch (ev->type) {

        case EV_MOUSE_MOVE:
            mouse_move();
            break;

        case EV_MOUSE_DOWN:
            mouse_down(ev);
            break;

        case EV_MOUSE_UP:
            drag_mode = DRAG_NONE;
            drag_win = NULL;
            need_redraw = 1;
            break;

        case EV_WHEEL: {

            struct window *win = window_at(mouse_x, mouse_y);

            if (win && win->wheel) {

                win->wheel(win, ev->value);
                need_redraw = 1;
            }

            break;
        }

        case EV_KEY_PRESS:
            key_press(ev);
            break;
    }
}

/* ---------- wallpaper (drawn once, then reused) ---------- */

void wm_draw_wallpaper(void) {

    gfx_noclip();

    gradient(0, 0, screen_w, screen_h, 0x0B1A33, 0x1D3B61);

    /* a warm saffron glow in the lower right, like a sunrise */
    int gx = screen_w * 4 / 5, gy = screen_h;
    double reach = screen_w * 0.6;

    for (int y = screen_h / 3; y < screen_h; y++) {

        for (int x = screen_w / 4; x < screen_w; x++) {

            double d = sqrt((double)(x - gx) * (x - gx) + (double)(y - gy) * (y - gy));

            if (d < reach) {

                double k = 1.0 - d / reach;

                put_pixel(x, y, C_SAFFRON, (int)(k * k * 70));
            }
        }
    }

    const char *mark = "Bharat-linux";
    int mw = text_width(&font_big, mark);

    text_alpha(&font_big, screen_w - mw - 30, screen_h - TASKBAR_H - 60, mark, C_WHITE, 70);

    const char *sub = "Made in India. Written from scratch.";
    int sw = text_width(&font_ui, sub);

    text_alpha(&font_ui, screen_w - sw - 30, screen_h - TASKBAR_H - 26, sub, C_WHITE, 90);

    save_background();
}
