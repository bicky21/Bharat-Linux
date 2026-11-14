/*
 * term.c - the Terminal app.
 *
 * A terminal is two things:
 *   1. a "pseudo terminal" (pty): a pair of connected files. The shell
 *      uses one end as its keyboard and screen; we hold the other end.
 *   2. a small interpreter for the escape codes programs print, such as
 *      "\033[1;32m" (= switch to bold green). We keep a grid of letters
 *      and colours and draw it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "desktop.h"

#define MAX_PARAMS 8

/* the 16 standard terminal colours */
static const uint32_t palette[16] = {
    0x1E2127, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
    0x5C6370, 0xFF7B85, 0xB5E890, 0xFFD88A, 0x7CC4FF, 0xDE95F0, 0x6FD5E0, 0xFFFFFF
};

#define DEFAULT_FG 7
#define DEFAULT_BG 0

struct cell {
    char ch;
    uint8_t fg, bg;
};

struct term {
    int fd;              /* our end of the pty */
    pid_t pid;           /* the shell */
    int cols, rows;
    struct cell *grid;
    int cx, cy;          /* cursor */
    int dirty_lo, dirty_hi;   /* rows that changed since the last redraw */
    int saved_cx, saved_cy;
    int cursor_on;
    uint8_t fg, bg;
    int bold, reverse;

    /* escape code reader */
    enum { S_TEXT, S_ESC, S_CSI, S_OSC } state;
    int params[MAX_PARAMS];
    int nparams;
    int private_mode;
};

#define CELL_W 8
#define CELL_H (font_mono.height)
#define PAD 6

/* ---------- the grid ---------- */

static struct cell blank(struct term *t) {

    return (struct cell){' ', DEFAULT_FG, t->bg};
}

/* Remember that rows lo..hi must be drawn again. */
static void touch(struct term *t, int lo, int hi) {

    if (lo < t->dirty_lo) t->dirty_lo = lo;
    if (hi > t->dirty_hi) t->dirty_hi = hi;
}

static struct cell *at(struct term *t, int x, int y) {

    return &t->grid[y * t->cols + x];
}

static void clear_cells(struct term *t, int from, int to) {

    for (int i = from; i < to; i++)
        t->grid[i] = blank(t);

    if (to > from)
        touch(t, from / t->cols, (to - 1) / t->cols);
}

static void scroll_up(struct term *t) {

    touch(t, 0, t->rows - 1);

    memmove(t->grid, t->grid + t->cols, sizeof(struct cell) * t->cols * (t->rows - 1));
    clear_cells(t, t->cols * (t->rows - 1), t->cols * t->rows);
}

static void scroll_down(struct term *t) {

    touch(t, 0, t->rows - 1);

    memmove(t->grid + t->cols, t->grid, sizeof(struct cell) * t->cols * (t->rows - 1));
    clear_cells(t, 0, t->cols);
}

static void new_line(struct term *t) {

    if (t->cy + 1 >= t->rows)
        scroll_up(t);
    else
        t->cy++;
}

static void clamp_cursor(struct term *t) {

    if (t->cx < 0) t->cx = 0;
    if (t->cy < 0) t->cy = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void put_char(struct term *t, char ch) {

    if (t->cx >= t->cols) {

        t->cx = 0;
        new_line(t);
    }

    uint8_t fg = t->fg, bg = t->bg;

    if (t->bold && fg < 8)
        fg += 8;              /* bold = the bright version of the colour */

    if (t->reverse) {

        uint8_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    *at(t, t->cx, t->cy) = (struct cell){ch, fg, bg};
    touch(t, t->cy, t->cy);
    t->cx++;
}

/* ---------- escape codes ---------- */

static int param(struct term *t, int i, int fallback) {

    return i < t->nparams && t->params[i] > 0 ? t->params[i] : fallback;
}

static void set_colours(struct term *t) {

    if (t->nparams == 0)
        t->nparams = 1, t->params[0] = 0;

    for (int i = 0; i < t->nparams; i++) {

        int p = t->params[i];

        if (p == 0) {

            t->fg = DEFAULT_FG;
            t->bg = DEFAULT_BG;
            t->bold = 0;
            t->reverse = 0;
        }
        else if (p == 1) t->bold = 1;
        else if (p == 22) t->bold = 0;
        else if (p == 7) t->reverse = 1;
        else if (p == 27) t->reverse = 0;
        else if (p >= 30 && p <= 37) t->fg = p - 30;
        else if (p == 39) t->fg = DEFAULT_FG;
        else if (p >= 40 && p <= 47) t->bg = p - 40;
        else if (p == 49) t->bg = DEFAULT_BG;
        else if (p >= 90 && p <= 97) t->fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) t->bg = p - 100 + 8;
    }
}

static void run_csi(struct term *t, char final) {

    int n = param(t, 0, 1);
    int pos = t->cy * t->cols + t->cx;
    int total = t->cols * t->rows;

    switch (final) {

        case 'm': set_colours(t); break;
        case 'A': t->cy -= n; break;
        case 'B': t->cy += n; break;
        case 'C': t->cx += n; break;
        case 'D': t->cx -= n; break;
        case 'G': t->cx = n - 1; break;
        case 'd': t->cy = n - 1; break;

        case 'H':
        case 'f':
            t->cy = param(t, 0, 1) - 1;
            t->cx = param(t, 1, 1) - 1;
            break;

        case 'J': {

            int mode = t->nparams ? t->params[0] : 0;

            if (mode == 0) clear_cells(t, pos, total);
            else if (mode == 1) clear_cells(t, 0, pos + 1);
            else clear_cells(t, 0, total);

            break;
        }

        case 'K': {

            int mode = t->nparams ? t->params[0] : 0;
            int start = t->cy * t->cols;

            if (mode == 0) clear_cells(t, pos, start + t->cols);
            else if (mode == 1) clear_cells(t, start, pos + 1);
            else clear_cells(t, start, start + t->cols);

            break;
        }

        case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
        case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;

        case 'h':
        case 'l':
            if (t->private_mode && param(t, 0, 0) == 25)
                t->cursor_on = final == 'h';
            break;
    }

    clamp_cursor(t);
}

static void feed(struct term *t, unsigned char c) {

    switch (t->state) {

        case S_TEXT:

            if (c == 27) t->state = S_ESC;
            else if (c == '\r') t->cx = 0;
            else if (c == '\n') new_line(t);
            else if (c == '\b') { if (t->cx > 0) t->cx--; }
            else if (c == '\t') { t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) t->cx = t->cols - 1; }
            else if (c >= 32 && c < 127) put_char(t, (char)c);
            else if (c >= 0xC0) put_char(t, '?');    /* start of a non-English letter */

            /* other control bytes and UTF-8 continuation bytes: ignore */
            break;

        case S_ESC:

            t->state = S_TEXT;

            if (c == '[') {

                t->state = S_CSI;
                t->nparams = 0;
                t->private_mode = 0;
                memset(t->params, 0, sizeof(t->params));
            }
            else if (c == ']') t->state = S_OSC;
            else if (c == '7') { t->saved_cx = t->cx; t->saved_cy = t->cy; }
            else if (c == '8') { t->cx = t->saved_cx; t->cy = t->saved_cy; clamp_cursor(t); }
            else if (c == 'M') { if (t->cy == 0) scroll_down(t); else t->cy--; }
            else if (c == 'c') { clear_cells(t, 0, t->cols * t->rows); t->cx = t->cy = 0; }

            break;

        case S_CSI:

            if (c == '?') {

                t->private_mode = 1;
            }
            else if (c >= '0' && c <= '9') {

                if (t->nparams == 0)
                    t->nparams = 1;

                int *p = &t->params[t->nparams - 1];

                if (*p < 10000)
                    *p = *p * 10 + (c - '0');
            }
            else if (c == ';') {

                if (t->nparams == 0)
                    t->nparams = 1;

                if (t->nparams < MAX_PARAMS)
                    t->nparams++;
            }
            else if (c >= 0x40 && c <= 0x7E) {

                run_csi(t, (char)c);
                t->state = S_TEXT;
            }

            break;

        case S_OSC:

            /* window title and similar: ends with BEL or ESC */
            if (c == 7) t->state = S_TEXT;
            else if (c == 27) t->state = S_ESC;

            break;
    }
}

/* ---------- size ---------- */

static void fit_to_window(struct window *win) {

    struct term *t = win->data;
    int x, y, w, h;

    content_box(win, &x, &y, &w, &h);

    int cols = (w - 2 * PAD) / CELL_W;
    int rows = (h - 2 * PAD) / CELL_H;

    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;

    if (cols == t->cols && rows == t->rows && t->grid)
        return;

    struct cell *grid = malloc(sizeof(struct cell) * cols * rows);

    if (!grid)
        return;

    for (int i = 0; i < cols * rows; i++)
        grid[i] = blank(t);

    /* keep the bottom part of the old text, where the cursor is */
    int shift = t->grid && t->cy >= rows ? t->cy - rows + 1 : 0;

    for (int r = 0; t->grid && r < rows && r + shift < t->rows; r++) {

        for (int c = 0; c < cols && c < t->cols; c++)
            grid[r * cols + c] = t->grid[(r + shift) * t->cols + c];
    }

    free(t->grid);

    t->grid = grid;
    t->cols = cols;
    t->rows = rows;
    t->cy -= shift;

    clamp_cursor(t);

    /* tell the programs inside the new size */
    struct winsize ws = {.ws_row = rows, .ws_col = cols};

    if (t->fd >= 0)
        ioctl(t->fd, TIOCSWINSZ, &ws);
}

/* ---------- window callbacks ---------- */

static void term_draw(struct window *win, int x, int y, int w, int h) {

    struct term *t = win->data;

    fill(x, y, w, h, palette[DEFAULT_BG]);

    for (int r = 0; r < t->rows; r++) {

        for (int c = 0; c < t->cols; c++) {

            struct cell *cell = at(t, c, r);
            int px = x + PAD + c * CELL_W;
            int py = y + PAD + r * CELL_H;

            if (cell->bg != DEFAULT_BG)
                fill(px, py, CELL_W, CELL_H, palette[cell->bg]);

            if (cell->ch != ' ')
                text_char(&font_mono, px, py, cell->ch, palette[cell->fg]);
        }
    }

    if (t->cursor_on && t->fd >= 0) {

        int px = x + PAD + t->cx * CELL_W;
        int py = y + PAD + t->cy * CELL_H;

        fill_alpha(px, py, CELL_W, CELL_H, C_SAFFRON, 200);

        struct cell *cell = at(t, t->cx < t->cols ? t->cx : t->cols - 1, t->cy);

        if (cell->ch != ' ' && t->cx < t->cols)
            text_char(&font_mono, px, py, cell->ch, 0x000000);
    }
}

static void send(struct term *t, const char *s, size_t n) {

    if (t->fd >= 0 && write(t->fd, s, n) < 0) {
        /* the shell is gone; term_readable will notice */
    }
}

static void term_key(struct window *win, struct event *ev) {

    struct term *t = win->data;
    int k = ev->key;

    switch (k) {

        case K_UP:     send(t, "\033[A", 3); return;
        case K_DOWN:   send(t, "\033[B", 3); return;
        case K_RIGHT:  send(t, "\033[C", 3); return;
        case K_LEFT:   send(t, "\033[D", 3); return;
        case K_HOME:   send(t, "\033[H", 3); return;
        case K_END:    send(t, "\033[F", 3); return;
        case K_PGUP:   send(t, "\033[5~", 4); return;
        case K_PGDN:   send(t, "\033[6~", 4); return;
        case K_DELETE: send(t, "\033[3~", 4); return;
    }

    if (k <= 0 || k > 127)
        return;

    char c = (char)k;

    if (c == '\b')
        c = 127;                     /* the terminal's "erase" key */
    else if ((ev->mods & MOD_CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (char)((c & 0x1f));      /* Ctrl+C = 3, Ctrl+D = 4, ... */

    send(t, &c, 1);
}

static int term_fd(struct window *win) {

    struct term *t = win->data;

    return t->fd;
}

static void term_readable(struct window *win) {

    struct term *t = win->data;
    unsigned char buf[4096];

    ssize_t n = read(t->fd, buf, sizeof(buf));

    if (n <= 0) {

        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;

        /* the shell has exited: close the window */
        win->want_close = 1;
        return;
    }

    int old_cy = t->cy;

    t->dirty_lo = t->rows;
    t->dirty_hi = -1;

    for (ssize_t i = 0; i < n; i++)
        feed(t, buf[i]);

    /* the cursor is drawn too: its old and new row change */
    touch(t, old_cy, old_cy);
    touch(t, t->cy, t->cy);

    /* redraw only the rows that changed, not the whole window */
    int x, y, w, h;

    content_box(win, &x, &y, &w, &h);

    if (t->dirty_lo <= t->dirty_hi)
        wm_damage(x, y + PAD + t->dirty_lo * CELL_H, w,
                  (t->dirty_hi - t->dirty_lo + 1) * CELL_H);
}

static void term_destroy(struct window *win) {

    struct term *t = win->data;

    if (t->fd >= 0)
        close(t->fd);       /* the shell gets "hang up" and exits */

    if (t->pid > 0) {

        kill(t->pid, SIGHUP);
        waitpid(t->pid, NULL, 0);
    }

    free(t->grid);
    free(t);
}

/* ---------- starting the shell ---------- */

static pid_t start_shell(int *master_out, int cols, int rows) {

    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        return -1;

    char *slave_name = ptsname(master);

    if (!slave_name) {

        close(master);
        return -1;
    }

    struct winsize ws = {.ws_row = rows, .ws_col = cols};

    ioctl(master, TIOCSWINSZ, &ws);

    pid_t pid = fork();

    if (pid < 0) {

        close(master);
        return -1;
    }

    if (pid == 0) {

        /* the new shell gets its own session with the pty as terminal */
        setsid();

        int slave = open(slave_name, O_RDWR);

        if (slave < 0)
            _exit(1);

        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);

        if (slave > 2)
            close(slave);

        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        setenv("TERM", "linux", 1);

        char *shell = getenv("SHELL");

        if (!shell)
            shell = "/bin/sh";

        execl(shell, shell, NULL);

        _exit(127);
    }

    fcntl(master, F_SETFL, O_NONBLOCK);

    *master_out = master;

    return pid;
}

void open_terminal(void) {

    struct window *win = wm_new_window("Terminal", 2 * PAD + 80 * CELL_W + 2,
                                       TITLE_H + 2 * PAD + 24 * CELL_H + 1);

    if (!win)
        return;

    struct term *t = calloc(1, sizeof(*t));

    if (!t) {

        win->want_close = 1;
        return;
    }

    t->fd = -1;
    t->fg = DEFAULT_FG;
    t->bg = DEFAULT_BG;
    t->cursor_on = 1;

    win->data = t;
    win->draw = term_draw;
    win->key = term_key;
    win->fd = term_fd;
    win->readable = term_readable;
    win->resized = fit_to_window;
    win->destroy = term_destroy;

    fit_to_window(win);

    t->pid = start_shell(&t->fd, t->cols, t->rows);

    if (t->pid < 0) {

        const char *msg = "Could not start a shell (no /dev/pts?)";

        for (const char *p = msg; *p; p++)
            put_char(t, *p);
    }
}
