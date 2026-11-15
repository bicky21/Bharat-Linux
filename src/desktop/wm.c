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
#include <sys/stat.h>

#include "desktop.h"

#define MAX_WINDOWS 16
#define GRIP 14              /* size of the resize corner */
#define BUTTON_W 40          /* width of a title-bar button */

static struct window *wins[MAX_WINDOWS];   /* [0] = bottom, last = top */
static int nwin;

int wm_quit;
int wm_exit_code;     /* what the login screen should do after we exit */

/*
 * Damaged areas: the boxes of the screen that must be drawn again.
 * Redrawing only these (instead of the whole screen) saves most of the
 * work, for example when only the mouse pointer moved.
 */
struct rect { int x, y, w, h; };

#define MAX_DAMAGE 8

static struct rect damage[MAX_DAMAGE];
static int ndamage;

/* the box of the button under the mouse (for hover colours) */
static struct rect hover;
static int last_minute = -1;

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

static int overlaps(struct rect *a, struct rect *b) {

    return a->x < b->x + b->w + 16 && b->x < a->x + a->w + 16 &&
           a->y < b->y + b->h + 16 && b->y < a->y + a->h + 16;
}

static struct rect join(struct rect a, struct rect b) {

    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;

    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

/* Mark a box of the screen as "must be drawn again". */
void wm_damage(int x, int y, int w, int h) {

    if (w <= 0 || h <= 0)
        return;

    struct rect r = {x, y, w, h};

    /* boxes that touch become one bigger box */
    for (int i = 0; i < ndamage; i++) {

        if (overlaps(&damage[i], &r)) {

            r = join(damage[i], r);
            damage[i] = damage[--ndamage];
            i = -1;              /* the bigger box may touch others now */
        }
    }

    if (ndamage == MAX_DAMAGE) {

        /* too many small boxes: one box around all of them */
        for (int i = 0; i < ndamage; i++)
            r = join(r, damage[i]);

        ndamage = 0;
    }

    damage[ndamage++] = r;
}

void wm_redraw(void) {

    ndamage = 0;
    wm_damage(0, 0, screen_w, screen_h);
}

/* a window plus its shadow */
void wm_damage_window(struct window *win) {

    if (!win->minimized)
        wm_damage(win->x - 2, win->y - 2, win->w + 8, win->h + 10);
}

int wm_needs_redraw(void) {

    return ndamage > 0;
}

int wm_wants_seconds(void) {

    for (int i = 0; i < nwin; i++) {

        if (wins[i]->tick && !wins[i]->minimized)
            return 1;
    }

    return 0;
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

    wm_redraw();
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
    wm_redraw();

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

int wm_window_alive(struct window *win) {

    for (int i = 0; i < nwin; i++)
        if (wins[i] == win && !win->want_close)
            return 1;

    return 0;
}

void wm_toggle_maximize(struct window *win) {

    if (win->maximized) {

        win->x = win->saved_x;
        win->y = win->saved_y;
        win->w = win->saved_w;
        win->h = win->saved_h;
        win->maximized = 0;
    }
    else {

        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->w;
        win->saved_h = win->h;
        win->x = 0;
        win->y = 0;
        win->w = screen_w;
        win->h = screen_h - TASKBAR_H;
        win->maximized = 1;
    }

    if (win->resized)
        win->resized(win);

    wm_redraw();
}

void wm_cleanup(void) {

    int out = 0;

    for (int i = 0; i < nwin; i++) {

        if (wins[i]->want_close) {

            destroy(wins[i]);
            wm_redraw();
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

        if (wins[i]->tick && !wins[i]->minimized) {

            wins[i]->tick(wins[i]);
            wm_damage_window(wins[i]);
        }
    }

    /* the clock in the taskbar changes once a minute */
    time_t t = time(NULL);
    int minute = (int)(t / 60);

    if (minute != last_minute) {

        last_minute = minute;
        wm_damage(screen_w - 160, screen_h - TASKBAR_H, 160, TASKBAR_H);
    }
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

void icon_disk(int x, int y, int s) {

    /* a hard disk seen from the front, with a green light */
    fill_round(x + s / 16, y + s / 4, s - s / 8, s / 2, s / 10, 0x57606A);
    fill_round(x + s / 8, y + s / 4 + s / 10, s - s / 4, s / 2 - s / 5, s / 14, 0x8C959F);
    fill_circle(x + s - s / 4, y + s / 2 + s / 8, s / 16 + 1, 0x56D364);
    fill(x + s / 5, y + s / 2 + s / 10, s / 3, 2, 0xD0D7DE);
}

void icon_network(int x, int y, int s, int connected) {

    /* three bars of a signal, like on a phone; grey with a line when off */
    uint32_t on = connected > 0 ? 0x56D364 : 0x8B949E;
    int bw = s / 5;

    for (int i = 0; i < 3; i++) {

        int bh = s * (i + 2) / 5;

        fill_round(x + s / 10 + i * (bw + s / 10), y + s - bh, bw, bh, 2, on);
    }

    if (connected <= 0)
        for (int i = 0; i < s; i++)
            fill(x + i, y + i, 2, 2, 0xCF222E);
}

/* ---------- desktop icons and the start menu entries ---------- */

struct launcher {
    const char *name;
    const char *short_name;      /* under the desktop icon */
    void (*icon)(int x, int y, int s);
    void (*open)(void);
};

void icon_tor(int x, int y, int s) {

    /* an onion: purple rings, the sign of Tor */
    int cx = x + s / 2, cy = y + s / 2 + s / 12, r = s / 2 - 2;
    uint32_t ring[] = {0x7D4698, 0xFFFFFF, 0x9A68B5, 0xFFFFFF, 0x59316B};

    for (int i = 0; i < 5; i++)
        fill_circle(cx, cy, r - i * r / 5, ring[i]);

    fill(cx - 1, y + 1, 3, s / 6, 0x56D364);       /* the green top */
}

static void icon_network_launcher(int x, int y, int s) {

    icon_network(x, y, s, 1);
}

static void open_home_files(void) {

    char *home = getenv("HOME");

    open_files(home ? home : "/");
}

static struct launcher launchers[] = {
    {"Terminal",       "Terminal",     icon_terminal, open_terminal},
    {"Files",          "Files",        icon_folder,   open_home_files},
    {"Tor Browser",    "Tor Browser",  icon_tor,      open_tor_browser},
    {"System Monitor", "Monitor",      icon_monitor,  open_monitor},
    {"Disks",          "Disks",        icon_disk,     open_disks},
    {"Network",        "Network",      icon_network_launcher, open_network},
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

/*
 * The bottom part of the menu. Started from the login screen (a
 * "session"), the desktop offers Log out / Restart / Power off: it exits
 * with a code and the login screen, which runs as root, does the rest.
 * Started by hand from a shell, it only offers to exit.
 */
struct system_item {
    const char *label;
    int exit_code;
    uint32_t color;
};

static struct system_item session_items[] = {
    {"Log out",   EXIT_LOGOUT,   0x57606A},
    {"Restart",   EXIT_REBOOT,   0x0969DA},
    {"Power off", EXIT_POWEROFF, 0xCF222E},
};

static struct system_item shell_items[] = {
    {"Exit desktop", EXIT_LOGOUT, 0xCF222E},
};

static struct system_item *system_items(int *count) {

    if (getenv("BHARAT_SESSION")) {

        *count = 3;
        return session_items;
    }

    *count = 1;
    return shell_items;
}

static int menu_items(void) {

    int n;

    system_items(&n);

    return LAUNCHERS + n;
}

static int menu_h(void) {

    return MENU_HEAD + menu_items() * MENU_ITEM + 18;
}

/* top of item i (the system items sit below a line) */
static int item_y(int menu_y, int i) {

    return menu_y + MENU_HEAD + 4 + i * MENU_ITEM + (i >= LAUNCHERS ? 8 : 0);
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
    text(&font_bold, x + 70, y + 18, "Bharat Linux", C_TEXT);

    char who[96];
    char *user = getenv("USER");

    snprintf(who, sizeof(who), "Signed in as %s", user ? user : "?");
    text(&font_ui, x + 70, y + 37, who, C_MUTED);

    fill(x + 12, y + MENU_HEAD - 1, MENU_W - 24, 1, C_BORDER);

    int count;
    struct system_item *sys = system_items(&count);

    fill(x + 12, item_y(y, LAUNCHERS) - 6, MENU_W - 24, 1, C_BORDER);

    for (int i = 0; i < LAUNCHERS + count; i++) {

        int iy = item_y(y, i);

        if (inside(mouse_x, mouse_y, x + 6, iy, MENU_W - 12, MENU_ITEM))
            fill_round(x + 6, iy, MENU_W - 12, MENU_ITEM, 6, C_SELECT);

        if (i < LAUNCHERS) {

            launchers[i].icon(x + 16, iy + 7, 24);
            text(&font_ui, x + 52, iy + 10, launchers[i].name, C_TEXT);
        }
        else {

            struct system_item *s = &sys[i - LAUNCHERS];

            /* "power" symbol: a ring with a bar */
            fill_circle(x + 28, iy + 19, 10, s->color);
            fill_circle(x + 28, iy + 19, 7, C_WHITE);
            fill(x + 26, iy + 6, 5, 10, C_WHITE);
            fill(x + 27, iy + 8, 3, 10, s->color);
            text(&font_ui, x + 52, iy + 10, s->label, C_TEXT);
        }
    }
}

/* Returns the item number under the mouse, or -1. */
static int menu_item_at(int px, int py) {

    int x, y;

    menu_box(&x, &y);

    for (int i = 0; i < menu_items(); i++) {

        if (inside(px, py, x + 6, item_y(y, i), MENU_W - 12, MENU_ITEM))
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

/* where the network icon sits in the taskbar */
#define NET_W 36

static int net_icon_x(void) {

    return screen_w - 14 - 86 - NET_W;
}


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

    /* network: green when connected */
    int nx = net_icon_x();
    int state = net_connected();

    if (inside(mouse_x, mouse_y, nx, y + 4, NET_W, TASKBAR_H - 6))
        fill_round(nx, y + 5, NET_W, TASKBAR_H - 8, 6, 0x30363D);

    icon_network(nx + 6, y + 9, 24, state);

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

    fit_text(&font_bold, win->title, w - 3 * BUTTON_W - 20, title, sizeof(title));
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

    /* maximise button: a square (two squares when maximised) */
    int zx = cx - BUTTON_W;

    title_color = active ? C_WHITE : 0x57606A;

    if (inside(mouse_x, mouse_y, zx, y, BUTTON_W, TITLE_H - 1))
        fill(zx, y, BUTTON_W, TITLE_H - 1, active ? 0x3B5488 : 0xD0D7DE);

    if (win->maximized) {

        frame(zx + 17, y + 8, 9, 9, title_color);
        fill(zx + 14, y + 11, 9, 9, active ? 0x22355E : 0xE6E9ED);
        frame(zx + 14, y + 11, 9, 9, title_color);
    }
    else {

        frame(zx + 14, y + 9, 12, 11, title_color);
        fill(zx + 14, y + 9, 12, 2, title_color);
    }

    /* minimise button (_) */
    int mx = zx - BUTTON_W;

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

static void draw_scene(void) {

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

    popup_draw();

    draw_cursor(mouse_x, mouse_y);
}

/* Draw every damaged box again, then copy just those boxes to the screen. */
void wm_draw(void) {

    for (int i = 0; i < ndamage; i++) {

        struct rect *r = &damage[i];

        gfx_area(r->x, r->y, r->w, r->h);
        draw_scene();
        gfx_flush_rect(r->x, r->y, r->w, r->h);
    }

    ndamage = 0;

    gfx_area(0, 0, screen_w, screen_h);
}

/* ---------- mouse and keyboard ---------- */

static void launch(int i) {

    menu_open = 0;

    int count;
    struct system_item *sys = system_items(&count);

    if (i >= 0 && i < LAUNCHERS) {

        launchers[i].open();
    }
    else if (i >= LAUNCHERS && i < LAUNCHERS + count) {

        wm_exit_code = sys[i - LAUNCHERS].exit_code;
        wm_quit = 1;
    }

    wm_redraw();
}

/*
 * Tor Browser is the Tor Project's own program, which needs the X11
 * display system. The desktop closes, the login screen starts it full
 * screen (x11-session), and the desktop comes back when it closes.
 */
void open_tor_browser(void) {

    if (access("/opt/tor-browser/Browser", F_OK) != 0 || access("/usr/lib/xorg/Xorg", X_OK) != 0) {

        dialog_info("Tor Browser",
                    "Not installed. Add it with: make torbrowser");
        return;
    }

    wm_exit_code = EXIT_TOR;
    wm_quit = 1;
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

/* ---------- right-click menus of the desktop itself ---------- */

static void act_minimize(void *ctx, int arg) { (void)arg; ((struct window *)ctx)->minimized = 1; }
static void act_maximize(void *ctx, int arg) { (void)arg; wm_toggle_maximize(ctx); }
static void act_close(void *ctx, int arg) { (void)arg; ((struct window *)ctx)->want_close = 1; }
static void act_restore(void *ctx, int arg) { (void)arg; raise_window(ctx); }

static void window_menu(struct window *win) {

    popup_begin(win);

    if (win->minimized)
        popup_add("Show", act_restore, 0, 1);
    else
        popup_add("Minimise", act_minimize, 0, 1);

    popup_add(win->maximized ? "Restore size" : "Maximise", act_maximize, 0, 1);
    popup_line();
    popup_add("Close", act_close, 0, 1);
    popup_show();
}

static void act_launch(void *ctx, int arg) { (void)ctx; launchers[arg].open(); }
static void act_wallpaper(void *ctx, int arg) { (void)ctx; wm_set_wallpaper(arg); }

static void desktop_menu(void) {

    popup_begin(NULL);
    popup_add("Open Terminal", act_launch, 0, 1);
    popup_add("Files", act_launch, 1, 1);
    popup_add("Tor Browser", act_launch, 2, 1);
    popup_add("Network", act_launch, LAUNCHERS - 2, 1);
    popup_add("Disks", act_launch, 4, 1);
    popup_add("System Monitor", act_launch, 3, 1);
    popup_line();
    popup_add("Background: Sunrise", act_wallpaper, 0, 1);
    popup_add("Background: Tiranga", act_wallpaper, 1, 1);
    popup_add("Background: Night", act_wallpaper, 2, 1);
    popup_add("Background: Forest", act_wallpaper, 3, 1);
    popup_line();
    popup_add("About Bharat Linux", act_launch, LAUNCHERS - 1, 1);
    popup_show();
}

static void icon_menu(int icon) {

    popup_begin(NULL);
    popup_add("Open", act_launch, icon, 1);
    popup_show();
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

    wm_redraw();

    /* 0. a right-click menu is open: it gets the click */
    if (popup_click())
        return;

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

        if (px >= net_icon_x() && px < net_icon_x() + NET_W) {

            open_network();
            return;
        }

        int i = (px - START_W) / task_button_w();

        if (px >= START_W && i >= 0 && i < nwin && ev->button == 2) {

            window_menu(wins[i]);
            return;
        }

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

            if (ev->button == 2) {

                window_menu(win);
                return;
            }

            if (ev->button != 1)
                return;

            if (rx >= win->w - BUTTON_W) {

                win->want_close = 1;
            }
            else if (rx >= win->w - 2 * BUTTON_W) {

                wm_toggle_maximize(win);
            }
            else if (rx >= win->w - 3 * BUTTON_W) {

                win->minimized = 1;
            }
            else if (dbl) {

                wm_toggle_maximize(win);
            }
            else {

                drag_mode = DRAG_MOVE;
                drag_win = win;
                drag_dx = rx;
                drag_dy = ry;
            }

            return;
        }

        if (ev->button == 1 && !win->maximized && rx >= win->w - GRIP && ry >= win->h - GRIP) {

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

    if (ev->button == 2) {

        if (icon >= 0)
            icon_menu(icon);
        else
            desktop_menu();

        return;
    }

    if (icon >= 0 && dbl && ev->button == 1)
        launch(icon);
}

/* The box of the button under the point, if it changes colour on hover. */
static struct rect hover_box(int px, int py) {

    struct rect none = {0, 0, 0, 0};

    if (menu_open && in_menu(px, py)) {

        int i = menu_item_at(px, py);
        int x, y;

        if (i < 0)
            return none;

        menu_box(&x, &y);

        return (struct rect){x + 6, item_y(y, i), MENU_W - 12, MENU_ITEM};
    }

    if (py >= screen_h - TASKBAR_H) {

        if (px >= net_icon_x() && px < net_icon_x() + NET_W)
            return (struct rect){net_icon_x(), screen_h - TASKBAR_H, NET_W, TASKBAR_H};

        return px < START_W ? (struct rect){0, screen_h - TASKBAR_H, START_W, TASKBAR_H} : none;
    }

    struct window *win = window_at(px, py);

    if (!win || py >= win->y + TITLE_H)
        return none;

    if (px >= win->x + win->w - BUTTON_W)
        return (struct rect){win->x + win->w - BUTTON_W, win->y, BUTTON_W, TITLE_H};

    for (int b = 2; b <= 3; b++)
        if (px >= win->x + win->w - b * BUTTON_W)
            return (struct rect){win->x + win->w - b * BUTTON_W, win->y, BUTTON_W, TITLE_H};

    return none;
}

static void mouse_move(int old_x, int old_y) {

    /* the pointer itself: where it was, and where it is now */
    wm_damage(old_x, old_y, CURSOR_W, CURSOR_H);
    wm_damage(mouse_x, mouse_y, CURSOR_W, CURSOR_H);

    popup_hover();

    /* apps that react to the pointer (links in the browser) */
    if (drag_mode == DRAG_NONE && !popup_is_open()) {

        struct window *under = window_at(mouse_x, mouse_y);

        if (under && under->hover && under == top_window()) {

            int bx, by, bw, bh;

            content_box(under, &bx, &by, &bw, &bh);

            if (mouse_x >= bx && mouse_y >= by && mouse_x < bx + bw && mouse_y < by + bh)
                under->hover(under, mouse_x - bx, mouse_y - by);
        }
    }

    /* a button that lights up (or stops lighting up) under the pointer */
    struct rect now = hover_box(mouse_x, mouse_y);

    if (now.x != hover.x || now.y != hover.y || now.w != hover.w || now.h != hover.h) {

        wm_damage(hover.x, hover.y, hover.w, hover.h);
        wm_damage(now.x, now.y, now.w, now.h);
        hover = now;
    }

    if (drag_mode == DRAG_MOVE && drag_win) {

        if (drag_win->maximized) {

            int keep = drag_dx * drag_win->saved_w / (drag_win->w ? drag_win->w : 1);

            wm_toggle_maximize(drag_win);
            drag_dx = keep;
        }

        wm_damage_window(drag_win);

        drag_win->x = mouse_x - drag_dx;
        drag_win->y = mouse_y - drag_dy;

        keep_on_screen(drag_win);
        wm_damage_window(drag_win);
    }
    else if (drag_mode == DRAG_RESIZE && drag_win) {

        int w = mouse_x - drag_win->x + drag_dx;
        int h = mouse_y - drag_win->y + drag_dy;

        if (w < drag_win->min_w) w = drag_win->min_w;
        if (h < drag_win->min_h) h = drag_win->min_h;

        if (w != drag_win->w || h != drag_win->h) {

            wm_damage_window(drag_win);

            drag_win->w = w;
            drag_win->h = h;

            if (drag_win->resized)
                drag_win->resized(drag_win);

            wm_damage_window(drag_win);
        }
    }
}

static void key_press(struct event *ev) {

    if (popup_key(ev))
        return;

    /* Ctrl+Alt+Backspace: leave the desktop (log out) */
    if ((ev->mods & MOD_CTRL) && (ev->mods & MOD_ALT) && ev->key == '\b') {

        wm_exit_code = EXIT_LOGOUT;
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

        wm_redraw();
        return;
    }

    if (ev->key == 27 && menu_open) {

        menu_open = 0;
        wm_redraw();
        return;
    }

    if (top && top->key) {

        top->key(top, ev);

        /* the Terminal redraws itself when the shell answers */
        if (!top->fd)
            wm_damage_window(top);
    }
}

void wm_event(struct event *ev) {

    switch (ev->type) {

        case EV_MOUSE_MOVE:
            mouse_move(ev->value >> 16, ev->value & 0xffff);
            break;

        case EV_MOUSE_DOWN:
            mouse_down(ev);
            break;

        case EV_MOUSE_UP:
            drag_mode = DRAG_NONE;
            drag_win = NULL;
            break;

        case EV_WHEEL: {

            struct window *win = window_at(mouse_x, mouse_y);

            if (win && win->wheel) {

                win->wheel(win, ev->value);
                wm_damage_window(win);
            }

            break;
        }

        case EV_KEY_PRESS:
            key_press(ev);
            break;
    }
}

/* ---------- wallpaper (drawn once, then reused) ---------- */

static const struct {
    uint32_t top, bottom, glow;
} themes[] = {
    {0x0B1A33, 0x1D3B61, C_SAFFRON},     /* Sunrise: night blue with a saffron glow */
    {0x2A1A0A, 0x0E3B1E, 0xF0F0F0},      /* Tiranga: saffron-brown to green, white glow */
    {0x05070C, 0x161B26, 0x3B6FD8},      /* Night */
    {0x0B2318, 0x1F4D33, 0xC7E36B},      /* Forest */
};

static int theme = -1;

static void theme_file(char *out, size_t size) {

    const char *home = getenv("HOME");

    snprintf(out, size, "%s/.config/bharat/wallpaper", home ? home : "/tmp");
}

void wm_set_wallpaper(int t) {

    char path[512], dir[512];

    theme = t < 0 || t > 3 ? 0 : t;

    /* remember it for next time */
    theme_file(path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0700);
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "/bharat");
    mkdir(dir, 0700);

    FILE *f = fopen(path, "w");

    if (f) {

        fprintf(f, "%d\n", theme);
        fclose(f);
    }

    wm_draw_wallpaper();
    wm_redraw();
}

void wm_draw_wallpaper(void) {

    if (theme < 0) {

        char path[512];

        theme_file(path, sizeof(path));
        FILE *f = fopen(path, "r");

        if (!f || fscanf(f, "%d", &theme) != 1 || theme < 0 || theme > 3)
            theme = 0;

        if (f)
            fclose(f);
    }

    gfx_area(0, 0, screen_w, screen_h);

    gradient(0, 0, screen_w, screen_h, themes[theme].top, themes[theme].bottom);

    /* a warm glow in the lower right, like a sunrise */
    int gx = screen_w * 4 / 5, gy = screen_h;
    double reach = screen_w * 0.6;

    for (int y = screen_h / 3; y < screen_h; y++) {

        for (int x = screen_w / 4; x < screen_w; x++) {

            double d = sqrt((double)(x - gx) * (x - gx) + (double)(y - gy) * (y - gy));

            if (d < reach) {

                double k = 1.0 - d / reach;

                put_pixel(x, y, themes[theme].glow, (int)(k * k * 70));
            }
        }
    }

    const char *mark = "Bharat Linux";
    int mw = text_width(&font_big, mark);

    text_alpha(&font_big, screen_w - mw - 30, screen_h - TASKBAR_H - 60, mark, C_WHITE, 70);

    const char *sub = "Made in India. Written from scratch.";
    int sw = text_width(&font_ui, sub);

    text_alpha(&font_ui, screen_w - sw - 30, screen_h - TASKBAR_H - 26, sub, C_WHITE, 90);

    save_background();
}
