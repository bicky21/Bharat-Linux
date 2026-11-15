/*
 * Bharat Desktop - a graphical desktop written from scratch.
 *
 * It draws straight into the Linux framebuffer (/dev/fb0) and reads the
 * keyboard and mouse straight from the kernel (/dev/input/event*).
 * No X11, no Wayland, no toolkit: every pixel is drawn by this code.
 *
 * Files:
 *   gfx.c    the screen: pixels, rectangles, text
 *   input.c  keyboard and mouse
 *   wm.c     window manager: windows, taskbar, menu, desktop icons
 *   term.c   the Terminal app
 *   apps.c   Files, Text Viewer, System Monitor, About
 *   disks.c  Disks, and opening USB sticks when they are plugged in
 *   main.c   start-up and the main loop
 */

#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>

/* ---------- colours (0xRRGGBB) ---------- */

#define C_SAFFRON   0xFF9933
#define C_SAFFRON_D 0xE07A1F
#define C_GREEN     0x2EA043
#define C_NAVY      0x1B2A4A
#define C_WHITE     0xFFFFFF
#define C_TEXT      0x1F2328
#define C_MUTED     0x6E7781
#define C_PANEL     0xF6F8FA
#define C_BORDER    0xD0D7DE
#define C_SELECT    0xFFE2C4

/* ---------- gfx.c ---------- */

struct font {
    const unsigned char *widths;
    const unsigned int *offsets;
    const unsigned char *alpha;
    int height;
    int ascent;
};

extern struct font font_ui, font_bold, font_mono, font_big, font_italic, font_h2, font_h3;

extern int screen_w, screen_h;

int  gfx_open(void);
void gfx_close(void);
void gfx_flush(void);
void gfx_flush_rect(int x, int y, int w, int h);

void gfx_area(int x, int y, int w, int h);   /* the part being redrawn */
void gfx_clip(int x, int y, int w, int h);   /* only draw inside this box */
void gfx_noclip(void);
int  gfx_visible(int x, int y, int w, int h);

#define CURSOR_W 12
#define CURSOR_H 19

void fill(int x, int y, int w, int h, uint32_t c);
void fill_alpha(int x, int y, int w, int h, uint32_t c, int alpha);
void fill_round(int x, int y, int w, int h, int r, uint32_t c);
void frame(int x, int y, int w, int h, uint32_t c);
void gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void fill_circle(int cx, int cy, int r, uint32_t c);
void line(int x0, int y0, int x1, int y1, uint32_t c);
void put_pixel(int x, int y, uint32_t c, int alpha);

int  text(struct font *f, int x, int y, const char *s, uint32_t c);
int  text_alpha(struct font *f, int x, int y, const char *s, uint32_t c, int alpha);
int  text_width(struct font *f, const char *s);
void text_char(struct font *f, int x, int y, char ch, uint32_t c);

void save_background(void);      /* remember the current picture... */
void restore_background(void);   /* ...and paint it back quickly */

struct image {
    int w, h;
    uint8_t *px;          /* red, green, blue, alpha for every pixel */
};

struct image *image_load(const char *path);
void draw_image(struct image *img, int x, int y);
void draw_image_alpha(struct image *img, int x, int y, int alpha);

void draw_logo(int x, int y, int size);
void draw_cursor(int x, int y);

/* ---------- input.c ---------- */

enum {
    EV_NONE,
    EV_KEY_PRESS,     /* key went down (or repeats) */
    EV_MOUSE_MOVE,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_WHEEL          /* value: +1 up, -1 down */
};

/* keys that are not normal characters */
enum {
    K_UP = 256, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DELETE, K_F4, K_F2
};

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

struct event {
    int type;
    int key;        /* character (ASCII) or one of K_... */
    int mods;       /* MOD_... */
    int button;     /* 1 left, 2 right, 3 middle */
    int value;
};

extern int mouse_x, mouse_y;

int  input_open(void);           /* returns number of devices */
void input_close(void);
void input_release(void);        /* let another program have the devices */
void input_take(void);           /* and take them back */
void input_rescan(void);         /* look for newly plugged devices */
int  input_watch_fd(void);       /* readable when /dev/input changes */
void input_watch_read(void);
int  input_fds(int *fds, int max);
int  input_read(int fd, struct event *out, int max);
int  input_has_mouse(void);

/* ---------- wm.c ---------- */

#define TITLE_H   30
#define TASKBAR_H 42

struct window {
    int x, y, w, h;          /* outer box, including the title bar */
    int min_w, min_h;
    char title[64];
    int minimized;
    int maximized;           /* fills the screen; saved_* is the old box */
    int saved_x, saved_y, saved_w, saved_h;
    int want_close;          /* the app asks to be closed */
    void *data;              /* the app's own state */

    /* everything below is optional (may be NULL) */
    void (*draw)(struct window *win, int x, int y, int w, int h);
    void (*key)(struct window *win, struct event *ev);
    void (*click)(struct window *win, int x, int y, int button, int dbl);
    void (*wheel)(struct window *win, int value);
    void (*hover)(struct window *win, int x, int y);   /* the mouse moved over it */
    void (*resized)(struct window *win);
    int  (*fd)(struct window *win);          /* a file to watch, or -1 */
    void (*readable)(struct window *win);    /* that file has data */
    void (*tick)(struct window *win);        /* once per second */
    void (*destroy)(struct window *win);
};

struct window *wm_new_window(const char *title, int w, int h);
void wm_redraw(void);              /* redraw the whole screen */
void wm_damage(int x, int y, int w, int h);   /* redraw only this box */
void wm_damage_window(struct window *win);
int  wm_wants_seconds(void);       /* a window needs a tick every second */
void wm_draw(void);
void wm_draw_wallpaper(void);
void wm_event(struct event *ev);
void wm_tick(void);
int  wm_poll_fds(int *fds, struct window **owners, int max);
void wm_cleanup(void);             /* close windows that want to close */
void wm_close_all(void);
int  wm_needs_redraw(void);
int  wm_window_alive(struct window *win);   /* not closed yet? */
void wm_toggle_maximize(struct window *win);
void wm_set_wallpaper(int theme);  /* 0..3, saved for next time */
extern int wm_quit;
extern int wm_exit_code;

/* how the desktop ends; the login screen (greeter) acts on it */
#define EXIT_LOGOUT   0
#define EXIT_POWEROFF 10
#define EXIT_REBOOT   11

void content_box(struct window *win, int *x, int *y, int *w, int *h);

/* icons, drawn with code (no image files) */
void icon_terminal(int x, int y, int s);
void icon_folder(int x, int y, int s);
void icon_file(int x, int y, int s);
void icon_monitor(int x, int y, int s);
void icon_about(int x, int y, int s);
void icon_disk(int x, int y, int s);
void icon_network(int x, int y, int s, int connected);
void icon_browser(int x, int y, int s);

/* ---------- popup.c: right-click menus, clipboard, dialogs ---------- */

void popup_begin(void *ctx);
void popup_add(const char *label, void (*action)(void *ctx, int arg), int arg, int enabled);
void popup_line(void);             /* a line before the next item */
void popup_show(void);             /* at the mouse pointer */
int  popup_is_open(void);
void popup_close(void);
void popup_draw(void);
void popup_hover(void);
int  popup_click(void);
int  popup_key(struct event *ev);

void clip_set_text(const char *text);
const char *clip_text(void);
void clip_set_files(const char *const *paths, int n, int cut);
int  clip_files(char ***paths, int *cut);
void clip_clear_files(void);

/* answer: the text typed (ask) or the button pressed (confirm) */
void dialog_ask(const char *title, const char *question, const char *initial, const char *button,
                struct window *owner, void (*done)(void *ctx, const char *answer), void *ctx);
void dialog_confirm(const char *title, const char *question, const char *button,
                    struct window *owner, void (*done)(void *ctx, const char *answer), void *ctx);
void dialog_info(const char *title, const char *text);

/* ---------- apps ---------- */

void open_terminal(void);
void open_files(const char *path);
void open_viewer(const char *path);
void open_monitor(void);
void open_about(void);
void open_disks(void);
/* a Terminal that runs one command (shell -c) */
void open_terminal_run(const char *title, const char *command);
void open_terminal_at(const char *dir);
void open_network(void);
void open_browser(const char *url);    /* NULL: the start page */

/* network.c: the network icon in the taskbar */
int  net_watch_fd(void);
void net_watch_read(void);
int  net_connected(void);          /* 1 connected, 0 not, -1 no card */
const char *net_summary(void);     /* "eth0 10.0.2.15" or why not */

/* disks.c: USB sticks plugged in while the desktop runs */
int  media_watch_fd(void);
void media_watch_read(void);
long long media_next(void);      /* ms until due, -1 none */
void media_run(void);

long long now_ms(void);

#endif
