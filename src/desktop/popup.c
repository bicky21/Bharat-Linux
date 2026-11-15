/*
 * popup.c - right-click menus, the clipboard, and small dialogs
 * (type a name, "are you sure?", information).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"

/* =====================================================================
 * right-click menus
 * ===================================================================== */

#define POPUP_MAX 16
#define POPUP_W 220
#define POPUP_ITEM 28

struct popup_item {
    char label[48];
    void (*action)(void *ctx, int arg);
    int arg;
    int disabled;
    int line_before;
};

static struct popup_item items[POPUP_MAX];
static int count;
static int open_now;
static int px, py;               /* top-left corner */
static void *context;
static int line_next;
static int hovered = -1;

void popup_begin(void *ctx) {

    count = 0;
    context = ctx;
    line_next = 0;
}

void popup_add(const char *label, void (*action)(void *ctx, int arg), int arg, int enabled) {

    if (count >= POPUP_MAX)
        return;

    struct popup_item *it = &items[count++];

    snprintf(it->label, sizeof(it->label), "%s", label);
    it->action = action;
    it->arg = arg;
    it->disabled = !enabled;
    it->line_before = line_next && count > 1;
    line_next = 0;
}

void popup_line(void) {

    line_next = 1;
}

static int item_top(int i) {

    int y = py + 6;

    for (int k = 0; k < i; k++)
        y += POPUP_ITEM + (items[k + 1].line_before ? 9 : 0);

    return y + (items[i].line_before ? 9 : 0) * 0;
}

static int popup_h(void) {

    int h = 12;

    for (int i = 0; i < count; i++)
        h += POPUP_ITEM + (items[i].line_before ? 9 : 0);

    return h;
}

/* opens the menu at the mouse pointer, kept inside the screen */
void popup_show(void) {

    if (!count)
        return;

    px = mouse_x;
    py = mouse_y;

    if (px + POPUP_W > screen_w - 4)
        px = screen_w - 4 - POPUP_W;

    if (py + popup_h() > screen_h - TASKBAR_H - 4)
        py = screen_h - TASKBAR_H - 4 - popup_h();

    if (py < 4)
        py = 4;

    open_now = 1;
    hovered = -1;
    wm_damage(px - 4, py - 4, POPUP_W + 12, popup_h() + 12);
}

int popup_is_open(void) {

    return open_now;
}

void popup_close(void) {

    if (!open_now)
        return;

    open_now = 0;
    wm_damage(px - 4, py - 4, POPUP_W + 12, popup_h() + 12);
}

static int row_y(int i) {

    int y = py + 6;

    for (int k = 0; k <= i; k++) {

        if (items[k].line_before)
            y += 9;

        if (k < i)
            y += POPUP_ITEM;
    }

    return y;
}

static int item_at(int x, int y) {

    if (x < px || x >= px + POPUP_W)
        return -1;

    for (int i = 0; i < count; i++) {

        int ry = row_y(i);

        if (y >= ry && y < ry + POPUP_ITEM)
            return i;
    }

    return -1;
}

void popup_draw(void) {

    if (!open_now)
        return;

    int h = popup_h();

    fill_alpha(px + 3, py + 5, POPUP_W, h, 0x000000, 50);
    fill_round(px, py, POPUP_W, h, 8, C_WHITE);
    frame(px, py, POPUP_W, h, C_BORDER);

    for (int i = 0; i < count; i++) {

        int y = row_y(i);

        if (items[i].line_before)
            fill(px + 10, y - 5, POPUP_W - 20, 1, C_BORDER);

        if (i == hovered && !items[i].disabled)
            fill_round(px + 4, y, POPUP_W - 8, POPUP_ITEM, 5, C_SELECT);

        text(&font_ui, px + 16, y + 6, items[i].label, items[i].disabled ? 0xAFB8C1 : C_TEXT);
    }

    (void)item_top;
}

/* the mouse moved: light up the item under it */
void popup_hover(void) {

    if (!open_now)
        return;

    int now = item_at(mouse_x, mouse_y);

    if (now != hovered) {

        hovered = now;
        wm_damage(px, py, POPUP_W, popup_h());
    }
}

/* a click while the menu is open: returns 1 (the click is used up) */
int popup_click(void) {

    if (!open_now)
        return 0;

    int i = item_at(mouse_x, mouse_y);

    popup_close();

    if (i >= 0 && !items[i].disabled && items[i].action)
        items[i].action(context, items[i].arg);

    wm_redraw();

    return 1;
}

/* keys while the menu is open: arrows, Enter, Esc */
int popup_key(struct event *ev) {

    if (!open_now)
        return 0;

    if (ev->key == 27) {

        popup_close();
    }
    else if (ev->key == K_DOWN || ev->key == K_UP) {

        int step = ev->key == K_DOWN ? 1 : count - 1;

        for (int k = 0; k < count; k++) {

            hovered = ((hovered < 0 ? (step == 1 ? -1 : 0) : hovered) + step) % count;

            if (!items[hovered].disabled)
                break;
        }

        wm_damage(px, py, POPUP_W, popup_h());
    }
    else if (ev->key == '\r' && hovered >= 0 && !items[hovered].disabled) {

        struct popup_item it = items[hovered];

        popup_close();

        if (it.action)
            it.action(context, it.arg);

        wm_redraw();
    }

    return 1;
}

/* =====================================================================
 * the clipboard: text, or files (copied or cut) - shared by all apps
 * ===================================================================== */

static char *clip_text_value;
static char **clip_paths;
static int clip_count, clip_cut;

void clip_set_text(const char *text) {

    free(clip_text_value);
    clip_text_value = text ? strdup(text) : NULL;
}

const char *clip_text(void) {

    return clip_text_value;
}

void clip_set_files(const char *const *paths, int n, int cut) {

    for (int i = 0; i < clip_count; i++)
        free(clip_paths[i]);

    free(clip_paths);
    clip_paths = n ? calloc((size_t)n, sizeof(char *)) : NULL;
    clip_count = 0;
    clip_cut = cut;

    for (int i = 0; clip_paths && i < n; i++)
        clip_paths[clip_count++] = strdup(paths[i]);

    /* the paths also go into the text clipboard (paste into a Terminal) */
    if (n == 1)
        clip_set_text(paths[0]);
}

int clip_files(char ***paths, int *cut) {

    *paths = clip_paths;
    *cut = clip_cut;

    return clip_count;
}

void clip_clear_files(void) {

    clip_set_files(NULL, 0, 0);
}

/* =====================================================================
 * dialogs: small windows that ask something
 * ===================================================================== */

#define DIALOG_W 400

enum { D_ASK, D_CONFIRM, D_INFO };

struct dialog {
    int kind;
    char message[6][96];
    int lines;
    char value[256];
    char button[24];
    void (*done)(void *ctx, const char *answer);
    void *ctx;
    struct window *owner;      /* the window that asked (may close first) */
};

static int dialog_body_h(struct dialog *d) {

    return 20 + d->lines * 20 + (d->kind == D_ASK ? 46 : 0);
}

static void dialog_buttons(struct dialog *d, int w, int h, int *ok_x, int *cancel_x, int *by) {

    *by = h - 46;
    *ok_x = w - 116;
    *cancel_x = d->kind == D_INFO ? -1 : w - 224;
}

static void dialog_draw(struct window *win, int x, int y, int w, int h) {

    struct dialog *d = win->data;
    int ok_x, cancel_x, by;

    fill(x, y, w, h, C_WHITE);

    for (int i = 0; i < d->lines; i++)
        text(i == 0 && d->kind != D_INFO ? &font_bold : &font_ui, x + 18, y + 16 + i * 20, d->message[i], C_TEXT);

    if (d->kind == D_ASK) {

        int fy = y + 22 + d->lines * 20;
        char shown[256];
        int tw;

        fill_round(x + 18, fy, w - 36, 32, 6, C_SAFFRON);
        fill_round(x + 20, fy + 2, w - 40, 28, 5, C_WHITE);

        /* the end of long text stays visible */
        snprintf(shown, sizeof(shown), "%s", d->value);

        const char *start = shown;

        while (*start && text_width(&font_ui, start) > w - 60)
            start++;

        text(&font_ui, x + 28, fy + 7, start, C_TEXT);
        tw = text_width(&font_ui, start);
        fill(x + 29 + tw, fy + 7, 2, 18, C_TEXT);
    }

    dialog_buttons(d, w, h, &ok_x, &cancel_x, &by);
    fill(x, y + by - 12, w, 1, C_BORDER);

    if (cancel_x >= 0) {

        fill_round(x + cancel_x, y + by, 100, 32, 6, C_PANEL);
        frame(x + cancel_x, y + by, 100, 32, C_BORDER);
        text(&font_ui, x + cancel_x + (100 - text_width(&font_ui, "Cancel")) / 2, y + by + 7, "Cancel", C_TEXT);
    }

    uint32_t colour = d->kind == D_CONFIRM && !strncmp(d->button, "Delete", 6) ? 0xCF222E : C_SAFFRON;

    fill_round(x + ok_x, y + by, 100, 32, 6, colour);
    text(&font_bold, x + ok_x + (100 - text_width(&font_bold, d->button)) / 2, y + by + 7, d->button, C_WHITE);
}

static void dialog_finish(struct window *win, int ok) {

    struct dialog *d = win->data;

    win->want_close = 1;

    if (ok && d->done && (!d->owner || wm_window_alive(d->owner)))
        d->done(d->ctx, d->value);
}

static void dialog_click(struct window *win, int x, int y, int button, int dbl) {

    struct dialog *d = win->data;
    int cx, cy, w, h, ok_x, cancel_x, by;

    (void)dbl;

    if (button != 1)
        return;

    content_box(win, &cx, &cy, &w, &h);
    dialog_buttons(d, w, h, &ok_x, &cancel_x, &by);

    if (y >= by && y < by + 32) {

        if (x >= ok_x && x < ok_x + 100)
            dialog_finish(win, 1);
        else if (cancel_x >= 0 && x >= cancel_x && x < cancel_x + 100)
            dialog_finish(win, 0);
    }
}

static void dialog_key(struct window *win, struct event *ev) {

    struct dialog *d = win->data;

    if (ev->key == '\r') {

        if (d->kind != D_ASK || d->value[0])
            dialog_finish(win, 1);
        return;
    }

    if (ev->key == 27) {

        dialog_finish(win, 0);
        return;
    }

    if (d->kind != D_ASK)
        return;

    size_t n = strlen(d->value);

    if (ev->key == '\b') {

        if (n)
            d->value[n - 1] = 0;
    }
    else if ((ev->mods & MOD_CTRL) && (ev->key == 'v' || ev->key == 'V')) {

        if (clip_text())
            snprintf(d->value + n, sizeof(d->value) - n, "%s", clip_text());
    }
    else if (ev->key >= 32 && ev->key < 127 && n + 1 < sizeof(d->value)) {

        d->value[n] = (char)ev->key;
        d->value[n + 1] = 0;
    }
}

static void dialog_destroy(struct window *win) {

    free(win->data);
}

/* splits text at \n into the message lines */
static struct window *new_dialog(int kind, const char *title, const char *message,
                                 const char *button, struct window *owner) {

    struct dialog *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;

    d->kind = kind;
    d->owner = owner;
    snprintf(d->button, sizeof(d->button), "%s", button);

    const char *p = message;

    while (*p && d->lines < 6) {

        size_t len = strcspn(p, "\n");

        snprintf(d->message[d->lines++], sizeof(d->message[0]), "%.*s", (int)(len < 95 ? len : 95), p);
        p += len + (p[len] == '\n');
    }

    struct window *win = wm_new_window(title, DIALOG_W, TITLE_H + dialog_body_h(d) + 70);

    if (!win) {

        free(d);
        return NULL;
    }

    /* in the middle of the screen */
    win->x = (screen_w - win->w) / 2;
    win->y = (screen_h - TASKBAR_H - win->h) / 2;
    win->data = d;
    win->draw = dialog_draw;
    win->click = dialog_click;
    win->key = dialog_key;
    win->destroy = dialog_destroy;
    win->min_w = win->w;
    win->min_h = win->h;

    return win;
}

void dialog_ask(const char *title, const char *question, const char *initial, const char *button,
                struct window *owner, void (*done)(void *ctx, const char *answer), void *ctx) {

    struct window *win = new_dialog(D_ASK, title, question, button, owner);

    if (!win)
        return;

    struct dialog *d = win->data;

    snprintf(d->value, sizeof(d->value), "%s", initial ? initial : "");
    d->done = done;
    d->ctx = ctx;
}

void dialog_confirm(const char *title, const char *question, const char *button,
                    struct window *owner, void (*done)(void *ctx, const char *answer), void *ctx) {

    struct window *win = new_dialog(D_CONFIRM, title, question, button, owner);

    if (!win)
        return;

    struct dialog *d = win->data;

    d->done = done;
    d->ctx = ctx;
}

void dialog_info(const char *title, const char *text) {

    new_dialog(D_INFO, title, text, "OK", NULL);
}
