/*
 * apps.c - the small built-in apps: Files, Text Viewer,
 * System Monitor and About.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include "desktop.h"

#define ROW_H 24

/* =====================================================================
 * Files
 * ===================================================================== */

struct entry {
    char name[256];
    int is_dir;
    long long size;
};

struct files {
    char path[PATH_MAX];
    struct entry *items;
    int count;
    int selected;
    int scroll;          /* first visible row */
    char error[128];
};

static int compare_entries(const void *a, const void *b) {

    const struct entry *x = a, *y = b;

    /* ".." first, then folders, then files, each in A-Z order */
    if (strcmp(x->name, "..") == 0) return -1;
    if (strcmp(y->name, "..") == 0) return 1;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;

    return strcmp(x->name, y->name);
}

static void join_path(const char *dir, const char *name, char *out, size_t size) {

    if (strcmp(dir, "/") == 0)
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

static void files_load(struct window *win, const char *path) {

    struct files *f = win->data;
    DIR *d = opendir(path);

    if (!d) {

        snprintf(f->error, sizeof(f->error), "Cannot open %s: %s", path, strerror(errno));
        return;
    }

    f->error[0] = 0;
    snprintf(f->path, sizeof(f->path), "%s", path);

    free(f->items);
    f->items = NULL;
    f->count = 0;

    int capacity = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {

        if (strcmp(e->d_name, ".") == 0)
            continue;

        if (strcmp(e->d_name, "..") == 0 && strcmp(path, "/") == 0)
            continue;

        if (f->count == capacity) {

            capacity = capacity ? capacity * 2 : 64;

            struct entry *bigger = realloc(f->items, sizeof(struct entry) * capacity);

            if (!bigger)
                break;

            f->items = bigger;
        }

        struct entry *it = &f->items[f->count++];
        char full[PATH_MAX];
        struct stat st;

        snprintf(it->name, sizeof(it->name), "%s", e->d_name);
        join_path(path, e->d_name, full, sizeof(full));

        if (stat(full, &st) == 0) {

            it->is_dir = S_ISDIR(st.st_mode);
            it->size = st.st_size;
        }
        else {

            it->is_dir = e->d_type == DT_DIR;
            it->size = 0;
        }
    }

    closedir(d);

    qsort(f->items, f->count, sizeof(struct entry), compare_entries);

    f->selected = 0;
    f->scroll = 0;

    char title[64];
    const char *last = strrchr(f->path, '/');

    snprintf(title, sizeof(title), "Files - %s", last && last[1] ? last + 1 : "/");
    snprintf(win->title, sizeof(win->title), "%s", title);
}

static void go_up(struct window *win) {

    struct files *f = win->data;
    char parent[PATH_MAX];

    snprintf(parent, sizeof(parent), "%s", f->path);

    char *slash = strrchr(parent, '/');

    if (!slash)
        return;

    if (slash == parent)
        parent[1] = 0;       /* parent of /home is / */
    else
        *slash = 0;

    files_load(win, parent);
}

static void open_entry(struct window *win, int i) {

    struct files *f = win->data;

    if (i < 0 || i >= f->count)
        return;

    struct entry *it = &f->items[i];

    if (strcmp(it->name, "..") == 0) {

        go_up(win);
        return;
    }

    char full[PATH_MAX];

    join_path(f->path, it->name, full, sizeof(full));

    if (it->is_dir)
        files_load(win, full);
    else
        open_viewer(full);
}

static void human_size(long long n, char *out, size_t size) {

    if (n < 1024) snprintf(out, size, "%lld B", n);
    else if (n < 1024 * 1024) snprintf(out, size, "%.1f KB", n / 1024.0);
    else snprintf(out, size, "%.1f MB", n / (1024.0 * 1024.0));
}

#define TOOLBAR_H 38

static int visible_rows(struct window *win) {

    int x, y, w, h;

    content_box(win, &x, &y, &w, &h);

    return (h - TOOLBAR_H) / ROW_H;
}

static void files_draw(struct window *win, int x, int y, int w, int h) {

    struct files *f = win->data;

    /* toolbar: "Up" button and the current folder */
    fill(x, y, w, TOOLBAR_H, C_PANEL);
    fill(x, y + TOOLBAR_H - 1, w, 1, C_BORDER);

    fill_round(x + 8, y + 7, 52, 24, 5, C_WHITE);
    frame(x + 8, y + 7, 52, 24, C_BORDER);
    text(&font_ui, x + 17, y + 10, "^ Up", C_TEXT);

    text(&font_bold, x + 72, y + 10, f->path, C_TEXT);

    int list_y = y + TOOLBAR_H;

    if (f->error[0]) {

        text(&font_ui, x + 12, list_y + 12, f->error, 0xCF222E);
        list_y += 34;
    }

    int rows = visible_rows(win);

    for (int r = 0; r < rows; r++) {

        int i = f->scroll + r;

        if (i >= f->count)
            break;

        struct entry *it = &f->items[i];
        int ry = list_y + r * ROW_H;

        if (i == f->selected)
            fill(x, ry, w, ROW_H, C_SELECT);

        if (it->is_dir)
            icon_folder(x + 10, ry + 3, 18);
        else
            icon_file(x + 10, ry + 3, 18);

        text(&font_ui, x + 36, ry + 4, it->name, C_TEXT);

        if (!it->is_dir) {

            char size[32];

            human_size(it->size, size, sizeof(size));
            text(&font_ui, x + w - 14 - text_width(&font_ui, size), ry + 4, size, C_MUTED);
        }
    }

    if (f->count == 0 && !f->error[0])
        text(&font_ui, x + 12, list_y + 10, "This folder is empty.", C_MUTED);

    (void)h;
}

static void keep_selection_visible(struct window *win) {

    struct files *f = win->data;
    int rows = visible_rows(win);

    if (f->selected < 0) f->selected = 0;
    if (f->selected >= f->count) f->selected = f->count - 1;
    if (f->selected < f->scroll) f->scroll = f->selected;
    if (f->selected >= f->scroll + rows) f->scroll = f->selected - rows + 1;
    if (f->scroll < 0) f->scroll = 0;
}

static void files_click(struct window *win, int x, int y, int button, int dbl) {

    struct files *f = win->data;

    if (button != 1)
        return;

    if (y < TOOLBAR_H) {

        if (x >= 8 && x < 60)
            go_up(win);

        return;
    }

    int i = f->scroll + (y - TOOLBAR_H - (f->error[0] ? 34 : 0)) / ROW_H;

    if (i < 0 || i >= f->count)
        return;

    f->selected = i;

    if (dbl)
        open_entry(win, i);
}

static void files_wheel(struct window *win, int value) {

    struct files *f = win->data;

    f->scroll -= value * 3;

    int max = f->count - visible_rows(win);

    if (f->scroll > max) f->scroll = max;
    if (f->scroll < 0) f->scroll = 0;
}

static void files_key(struct window *win, struct event *ev) {

    struct files *f = win->data;

    switch (ev->key) {

        case K_UP:   f->selected--; break;
        case K_DOWN: f->selected++; break;
        case K_PGUP: f->selected -= visible_rows(win); break;
        case K_PGDN: f->selected += visible_rows(win); break;
        case K_HOME: f->selected = 0; break;
        case K_END:  f->selected = f->count - 1; break;
        case '\r':   open_entry(win, f->selected); return;
        case '\b':   go_up(win); return;
        default:     return;
    }

    keep_selection_visible(win);
}

static void files_destroy(struct window *win) {

    struct files *f = win->data;

    free(f->items);
    free(f);
}

void open_files(const char *path) {

    struct window *win = wm_new_window("Files", 560, 440);

    if (!win)
        return;

    struct files *f = calloc(1, sizeof(*f));

    if (!f) {

        win->want_close = 1;
        return;
    }

    win->data = f;
    win->draw = files_draw;
    win->click = files_click;
    win->wheel = files_wheel;
    win->key = files_key;
    win->destroy = files_destroy;

    files_load(win, path);

    if (!f->path[0])
        files_load(win, "/");
}

/* =====================================================================
 * Text Viewer
 * ===================================================================== */

#define VIEW_MAX (256 * 1024)

struct viewer {
    char *text;
    char **lines;
    int count;
    int top;
    char message[160];
};

static void viewer_draw(struct window *win, int x, int y, int w, int h) {

    struct viewer *v = win->data;

    fill(x, y, w, h, 0xFBFBFB);

    if (v->message[0]) {

        text(&font_ui, x + 14, y + 14, v->message, C_MUTED);
        return;
    }

    int line_h = font_mono.height;
    int number_w = 48;

    fill(x, y, number_w, h, 0xEFF1F3);

    for (int r = 0; r * line_h < h; r++) {

        int i = v->top + r;

        if (i >= v->count)
            break;

        char number[16];

        snprintf(number, sizeof(number), "%d", i + 1);
        text(&font_mono, x + number_w - 8 - text_width(&font_mono, number),
             y + 4 + r * line_h, number, 0x8C959F);

        text(&font_mono, x + number_w + 8, y + 4 + r * line_h, v->lines[i], C_TEXT);
    }
}

static int viewer_rows(struct window *win) {

    int x, y, w, h;

    content_box(win, &x, &y, &w, &h);

    return h / font_mono.height;
}

static void viewer_scroll(struct window *win, int lines) {

    struct viewer *v = win->data;

    v->top += lines;

    int max = v->count - viewer_rows(win) + 1;

    if (v->top > max) v->top = max;
    if (v->top < 0) v->top = 0;
}

static void viewer_wheel(struct window *win, int value) {

    viewer_scroll(win, -value * 3);
}

static void viewer_key(struct window *win, struct event *ev) {

    int page = viewer_rows(win) - 1;

    switch (ev->key) {

        case K_UP:   viewer_scroll(win, -1); break;
        case K_DOWN: viewer_scroll(win, 1); break;
        case K_PGUP: viewer_scroll(win, -page); break;
        case K_PGDN:
        case ' ':    viewer_scroll(win, page); break;
        case K_HOME: viewer_scroll(win, -1000000); break;
        case K_END:  viewer_scroll(win, 1000000); break;
        case 27:     win->want_close = 1; break;
    }
}

static void viewer_destroy(struct window *win) {

    struct viewer *v = win->data;

    free(v->text);
    free(v->lines);
    free(v);
}

void open_viewer(const char *path) {

    const char *name = strrchr(path, '/');
    char title[64];

    snprintf(title, sizeof(title), "%s", name ? name + 1 : path);

    struct window *win = wm_new_window(title, 640, 460);

    if (!win)
        return;

    struct viewer *v = calloc(1, sizeof(*v));

    if (!v) {

        win->want_close = 1;
        return;
    }

    win->data = v;
    win->draw = viewer_draw;
    win->wheel = viewer_wheel;
    win->key = viewer_key;
    win->destroy = viewer_destroy;

    FILE *file = fopen(path, "rb");

    if (!file) {

        snprintf(v->message, sizeof(v->message), "Cannot open this file: %s", strerror(errno));
        return;
    }

    v->text = malloc(VIEW_MAX + 1);

    size_t n = v->text ? fread(v->text, 1, VIEW_MAX, file) : 0;

    fclose(file);

    if (!v->text)
        return;

    v->text[n] = 0;

    if (memchr(v->text, 0, n)) {

        snprintf(v->message, sizeof(v->message),
                 "This is not a text file (it is a program or picture).");
        return;
    }

    /* split into lines, in place */
    int capacity = 256;

    v->lines = malloc(sizeof(char *) * capacity);

    char *p = v->text;

    while (v->lines && *p) {

        if (v->count == capacity) {

            capacity *= 2;

            char **bigger = realloc(v->lines, sizeof(char *) * capacity);

            if (!bigger)
                break;

            v->lines = bigger;
        }

        v->lines[v->count++] = p;

        char *end = strchr(p, '\n');

        if (!end)
            break;

        *end = 0;
        p = end + 1;
    }

    /* tabs would show as "?" - turn them into a space */
    for (size_t i = 0; i < n; i++) {

        if (v->text[i] == '\t' || v->text[i] == '\r')
            v->text[i] = ' ';
    }

    if (v->count == 0)
        snprintf(v->message, sizeof(v->message), "This file is empty.");
}

/* =====================================================================
 * System Monitor
 * ===================================================================== */

#define HISTORY 60

struct monitor {
    unsigned long long last_total, last_idle;
    int cpu[HISTORY];        /* percent, newest last */
    long mem_total, mem_avail;    /* kB */
    double uptime;
    int processes;
    char kernel[160];
    char host[80];
};

static void read_cpu(struct monitor *m) {

    FILE *f = fopen("/proc/stat", "r");

    if (!f)
        return;

    unsigned long long v[8] = {0};

    int got = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);

    fclose(f);

    if (got < 4)
        return;

    unsigned long long total = 0;

    for (int i = 0; i < 8; i++)
        total += v[i];

    unsigned long long idle = v[3] + v[4];    /* idle + waiting for disk */

    int percent = 0;

    if (m->last_total && total > m->last_total) {

        unsigned long long dt = total - m->last_total;
        unsigned long long di = idle - m->last_idle;

        percent = (int)(100 - di * 100 / dt);
    }

    m->last_total = total;
    m->last_idle = idle;

    memmove(m->cpu, m->cpu + 1, sizeof(int) * (HISTORY - 1));
    m->cpu[HISTORY - 1] = percent;
}

static void read_memory(struct monitor *m) {

    FILE *f = fopen("/proc/meminfo", "r");

    if (!f)
        return;

    char key[64];
    long value;

    while (fscanf(f, "%63s %ld kB", key, &value) == 2) {

        if (strcmp(key, "MemTotal:") == 0) m->mem_total = value;
        if (strcmp(key, "MemAvailable:") == 0) m->mem_avail = value;
    }

    fclose(f);
}

static void monitor_tick(struct window *win) {

    struct monitor *m = win->data;

    read_cpu(m);
    read_memory(m);

    FILE *f = fopen("/proc/uptime", "r");

    if (f) {

        if (fscanf(f, "%lf", &m->uptime) != 1)
            m->uptime = 0;

        fclose(f);
    }

    m->processes = 0;

    DIR *d = opendir("/proc");

    if (d) {

        struct dirent *e;

        while ((e = readdir(d)) != NULL) {

            if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
                m->processes++;
        }

        closedir(d);
    }
}

static void bar(int x, int y, int w, int percent, uint32_t c) {

    fill_round(x, y, w, 12, 6, 0xE6E9ED);

    int fw = w * percent / 100;

    if (fw >= 12)
        fill_round(x, y, fw, 12, 6, c);
}

static void monitor_draw(struct window *win, int x, int y, int w, int h) {

    struct monitor *m = win->data;
    char s[160];
    int cy = y + 14;

    fill(x, y, w, h, C_WHITE);

    /* CPU */
    int cpu = m->cpu[HISTORY - 1];

    text(&font_bold, x + 16, cy, "Processor", C_TEXT);
    snprintf(s, sizeof(s), "%d%%", cpu);
    text(&font_bold, x + w - 16 - text_width(&font_bold, s), cy, s, C_TEXT);
    bar(x + 16, cy + 22, w - 32, cpu, C_SAFFRON);

    /* CPU history graph */
    int gx = x + 16, gy = cy + 44, gw = w - 32, gh = 90;

    fill_round(gx, gy, gw, gh, 6, 0x0D1117);

    for (int i = 1; i < 4; i++)
        fill(gx + 4, gy + i * gh / 4, gw - 8, 1, 0x21262D);

    for (int i = 1; i < HISTORY; i++) {

        int x0 = gx + 4 + (i - 1) * (gw - 8) / (HISTORY - 1);
        int x1 = gx + 4 + i * (gw - 8) / (HISTORY - 1);
        int y0 = gy + gh - 4 - m->cpu[i - 1] * (gh - 8) / 100;
        int y1 = gy + gh - 4 - m->cpu[i] * (gh - 8) / 100;

        line(x0, y0, x1, y1, 0x56D364);
        line(x0, y0 + 1, x1, y1 + 1, 0x56D364);
    }

    cy = gy + gh + 22;

    /* memory */
    long used = m->mem_total - m->mem_avail;
    int mem_percent = m->mem_total ? (int)(used * 100 / m->mem_total) : 0;

    text(&font_bold, x + 16, cy, "Memory", C_TEXT);
    snprintf(s, sizeof(s), "%ld MB of %ld MB", used / 1024, m->mem_total / 1024);
    text(&font_ui, x + w - 16 - text_width(&font_ui, s), cy, s, C_MUTED);
    bar(x + 16, cy + 22, w - 32, mem_percent, C_GREEN);

    cy += 52;

    /* facts */
    long up = (long)m->uptime;

    snprintf(s, sizeof(s), "%ldh %02ldm %02lds", up / 3600, (up / 60) % 60, up % 60);

    const char *labels[] = {"Computer", "Kernel", "Running for", "Processes"};
    char values[4][160];

    snprintf(values[0], sizeof(values[0]), "%s", m->host);
    snprintf(values[1], sizeof(values[1]), "%s", m->kernel);
    snprintf(values[2], sizeof(values[2]), "%s", s);
    snprintf(values[3], sizeof(values[3]), "%d", m->processes);

    for (int i = 0; i < 4; i++) {

        text(&font_ui, x + 16, cy + i * 24, labels[i], C_MUTED);
        text(&font_ui, x + 130, cy + i * 24, values[i], C_TEXT);
    }
}

void open_monitor(void) {

    struct window *win = wm_new_window("System Monitor", 440, 430);

    if (!win)
        return;

    struct monitor *m = calloc(1, sizeof(*m));

    if (!m) {

        win->want_close = 1;
        return;
    }

    struct utsname u;

    if (uname(&u) == 0) {

        snprintf(m->kernel, sizeof(m->kernel), "Linux %.64s", u.release);
        snprintf(m->host, sizeof(m->host), "%.60s", u.nodename);
    }

    win->data = m;
    win->draw = monitor_draw;
    win->tick = monitor_tick;
    win->min_w = 360;
    win->min_h = 400;

    monitor_tick(win);
}

/* =====================================================================
 * About
 * ===================================================================== */

static void about_draw(struct window *win, int x, int y, int w, int h) {

    (void)win;

    gradient(x, y, w, 110, 0xFFF4E8, C_WHITE);

    draw_logo(x + (w - 64) / 2, y + 22, 64);

    const char *lines[] = {
        "Bharat-linux",
        "Desktop 0.1",
        "",
        "An operating system made in India.",
        "Every window, letter and button here is",
        "drawn by our own code, straight to the screen.",
        "",
        "Kernel: Linux.  Fonts: DejaVu.",
    };

    int cy = y + 100;

    for (int i = 0; i < (int)(sizeof(lines) / sizeof(*lines)); i++) {

        struct font *f = i == 0 ? &font_big : i == 1 ? &font_bold : &font_ui;
        uint32_t c = i <= 1 ? C_TEXT : C_MUTED;
        int tw = text_width(f, lines[i]);

        text(f, x + (w - tw) / 2, cy, lines[i], c);

        cy += f->height + 4;
    }

    (void)h;
}

static void about_key(struct window *win, struct event *ev) {

    if (ev->key == 27 || ev->key == '\r')
        win->want_close = 1;
}

void open_about(void) {

    struct window *win = wm_new_window("About Bharat", 420, 360);

    if (!win)
        return;

    win->draw = about_draw;
    win->key = about_key;
}
