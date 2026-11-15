/*
 * disks.c - the Disks app: every disk and part, what is on it, where
 * it is open. USB sticks can be opened in Files or ejected; a new disk
 * can be added (it runs "sudo bharat-disk add" in a Terminal).
 *
 * It also notices USB sticks being plugged in (devd opens them in
 * /media/USER) and shows them in a Files window.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "desktop.h"
#include "../libdisk.h"

#define ROW_H    26
#define HEAD_H   34
#define FOOT_H   46
#define MAX_ROWS 48

struct disk_row {
    char name[32];
    int part;
    char size[24];
    char type[16];
    char label[64];
    char where[256];
    int removable;
};

struct disks {
    struct disk_row row[MAX_ROWS];
    int count;
    int ticks;
    char message[160];
    uint32_t message_colour;
};

static int disk_is_removable(const char *disk) {

    char path[128], link[512];

    snprintf(path, sizeof(path), "/sys/class/block/%.40s", disk);

    ssize_t n = readlink(path, link, sizeof(link) - 1);

    if (n > 0) {

        link[n] = 0;

        if (strstr(link, "/usb"))
            return 1;
    }

    snprintf(path, sizeof(path), "/sys/block/%.40s/removable", disk);

    return strncmp(disk, "sr", 2) != 0 && disk_read_number(path) == 1;
}

static void disks_load(struct disks *d) {

    struct blockdev list[MAX_ROWS];
    int n = list_blockdevs(list, MAX_ROWS);

    d->count = 0;

    for (int i = 0; i < n && d->count < MAX_ROWS; i++) {

        struct disk_row *r = &d->row[d->count++];
        char dev[64];
        struct fs_info fs;

        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%.31s", list[i].name);
        r->part = list[i].partno;
        disk_size_text(list[i].bytes, r->size, sizeof(r->size));
        r->removable = disk_is_removable(list[i].partno ? list[i].parent : list[i].name);

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);

        if (probe_fs(dev, &fs) == 0) {

            snprintf(r->type, sizeof(r->type), "%s", fs.type);
            snprintf(r->label, sizeof(r->label), "%s", fs.label);
        }
        else if (!list[i].partno) {

            snprintf(r->label, sizeof(r->label), "%.63s", list[i].model);
        }

        mount_point_of(dev, r->where, sizeof(r->where));
    }
}

/* the Open / Eject buttons of a row */
static void row_buttons(int w, int *open_x, int *eject_x) {

    *eject_x = w - 76;
    *open_x = w - 146;
}

static void button(int x, int y, int w, const char *label, int strong) {

    fill_round(x, y, w, 22, 5, strong ? C_SAFFRON : C_WHITE);
    frame(x, y, w, 22, strong ? C_SAFFRON_D : C_BORDER);
    text(&font_ui, x + (w - text_width(&font_ui, label)) / 2, y + 3, label, strong ? C_WHITE : C_TEXT);
}

static int can_open(const struct disk_row *r) {

    return r->where[0] == '/';
}

static int can_eject(const struct disk_row *r) {

    return r->removable && (r->where[0] == '/' || !r->part);
}

static void disks_draw(struct window *win, int x, int y, int w, int h) {

    struct disks *d = win->data;
    int cols[] = {12, 120, 200, 270, 400};

    fill(x, y, w, HEAD_H, C_PANEL);
    fill(x, y + HEAD_H - 1, w, 1, C_BORDER);

    const char *heads[] = {"Disk", "Size", "Type", "Name", "Open at"};

    for (int c = 0; c < 5; c++)
        text(&font_bold, x + cols[c], y + 9, heads[c], C_TEXT);

    int open_x, eject_x;

    row_buttons(w, &open_x, &eject_x);

    for (int i = 0; i < d->count; i++) {

        struct disk_row *r = &d->row[i];
        int ry = y + HEAD_H + i * ROW_H;

        if (ry + ROW_H > y + h - FOOT_H)
            break;

        if (!r->part)
            fill(x, ry, w, ROW_H, 0xFBFBFC);

        char name[40];

        snprintf(name, sizeof(name), "%s%s", r->part ? "  " : "", r->name);
        text(r->part ? &font_ui : &font_bold, x + cols[0], ry + 5, name, C_TEXT);
        text(&font_ui, x + cols[1], ry + 5, r->size, C_MUTED);
        text(&font_ui, x + cols[2], ry + 5, r->type, C_TEXT);
        text(&font_ui, x + cols[3], ry + 5, r->label, C_TEXT);

        int right = can_open(r) ? open_x : (can_eject(r) ? eject_x : w);
        char where[256];

        /* shorten long folder names so they stop before the buttons */
        snprintf(where, sizeof(where), "%s", r->where[0] ? r->where : (r->removable ? "USB" : ""));

        for (size_t len = strlen(where); len > 3 && text_width(&font_ui, where) > right - cols[4] - 10; len--)
            strcpy(where + len - 3, "..");

        text(&font_ui, x + cols[4], ry + 5, where, r->where[0] ? C_GREEN : C_MUTED);

        if (can_open(r))
            button(x + open_x, ry + 2, 62, "Open", 0);

        if (can_eject(r))
            button(x + eject_x, ry + 2, 66, "Eject", 0);
    }

    if (d->count == 0)
        text(&font_ui, x + 12, y + HEAD_H + 10, "No disks found.", C_MUTED);

    /* footer */
    int fy = y + h - FOOT_H;

    fill(x, fy, w, FOOT_H, C_PANEL);
    fill(x, fy, w, 1, C_BORDER);
    button(x + 12, fy + 12, 128, "Add a disk...", 1);

    if (d->message[0])
        text(&font_ui, x + 156, fy + 15, d->message, d->message_colour);
    else
        text(&font_ui, x + 156, fy + 15, "USB sticks open by themselves in /media.", C_MUTED);
}

/* asks devd to eject, like the eject command */
static void eject(struct disks *d, const char *dev) {

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    char msg[128], reply[300] = "";

    snprintf(a.sun_path, sizeof(a.sun_path), "/run/devd.sock");
    snprintf(msg, sizeof(msg), "eject %s\n", dev);

    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        write(s, msg, strlen(msg)) > 0) {

        ssize_t n = read(s, reply, sizeof(reply) - 1);

        reply[n > 0 ? n : 0] = 0;
    }

    if (s >= 0)
        close(s);

    reply[strcspn(reply, "\n")] = 0;

    if (!strncmp(reply, "ok ", 3)) {

        snprintf(d->message, sizeof(d->message), "%.159s", reply + 3);
        d->message_colour = C_GREEN;
    }
    else {

        snprintf(d->message, sizeof(d->message), "%.159s",
                 !strncmp(reply, "error ", 6) ? reply + 6 : "The device service is not answering.");
        d->message_colour = 0xCF222E;
    }

    disks_load(d);
}

static void disks_click(struct window *win, int x, int y, int button_nr, int dbl) {

    struct disks *d = win->data;
    int cx, cy, w, h;

    (void)dbl;

    if (button_nr != 1)
        return;

    content_box(win, &cx, &cy, &w, &h);

    if (y >= h - FOOT_H) {

        if (x >= 12 && x < 140)
            open_terminal_run("Add a disk",
                              "sudo bharat-disk add; echo; echo 'Press Enter to close this window.'; head -n 1 >/dev/null");
        return;
    }

    int i = (y - HEAD_H) / ROW_H;

    if (y < HEAD_H || i >= d->count)
        return;

    int open_x, eject_x;

    row_buttons(w, &open_x, &eject_x);

    struct disk_row *r = &d->row[i];
    char dev[64];

    snprintf(dev, sizeof(dev), "/dev/%.31s", r->name);

    if (can_open(r) && x >= open_x && x < open_x + 62)
        open_files(r->where);
    else if (can_eject(r) && x >= eject_x && x < eject_x + 66)
        eject(d, dev);
}

static void disks_tick(struct window *win) {

    struct disks *d = win->data;

    /* look again every 2 seconds: sticks come and go */
    if (++d->ticks % 2 == 0) {

        disks_load(d);
        wm_damage_window(win);
    }
}

static void disks_destroy(struct window *win) {

    free(win->data);
}

void open_disks(void) {

    struct window *win = wm_new_window("Disks", 660, 400);

    if (!win)
        return;

    struct disks *d = calloc(1, sizeof(*d));

    if (!d) {

        win->want_close = 1;
        return;
    }

    win->data = d;
    win->draw = disks_draw;
    win->click = disks_click;
    win->tick = disks_tick;
    win->destroy = disks_destroy;
    win->min_w = 560;
    win->min_h = 240;

    disks_load(d);
}

/* =====================================================================
 * USB sticks plugged in while the desktop runs
 * ===================================================================== */

static int media_fd = -1;
static int user_watch = -1;
static char media_dir[160];
static char pending[300];
static long long pending_at;

static void watch_user_folder(int show_existing) {

    if (user_watch >= 0)
        return;

    user_watch = inotify_add_watch(media_fd, media_dir, IN_CREATE | IN_ONLYDIR);

    /* the stick's folder may have been made before we started watching */
    DIR *d = show_existing && user_watch >= 0 ? opendir(media_dir) : NULL;
    struct dirent *e;

    while (d && (e = readdir(d)) != NULL) {

        if (e->d_name[0] == '.')
            continue;

        snprintf(pending, sizeof(pending), "%s/%.100s", media_dir, e->d_name);
        pending_at = now_ms() + 700;
    }

    if (d)
        closedir(d);
}

int media_watch_fd(void) {

    if (media_fd >= 0)
        return media_fd;

    const char *user = getenv("USER");

    if (!user)
        return -1;

    media_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    if (media_fd < 0)
        return -1;

    snprintf(media_dir, sizeof(media_dir), "/media/%.100s", user);

    /* /media/USER is made when the first stick arrives */
    inotify_add_watch(media_fd, "/media", IN_CREATE | IN_ONLYDIR);
    watch_user_folder(0);

    return media_fd;
}

void media_watch_read(void) {

    char buf[2048] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;

    while ((n = read(media_fd, buf, sizeof(buf))) > 0) {

        for (char *p = buf; p < buf + n;) {

            struct inotify_event *ev = (struct inotify_event *)p;

            if (ev->wd == user_watch && ev->len) {

                /* devd makes the folder, then opens the stick in it */
                snprintf(pending, sizeof(pending), "%s/%.100s", media_dir, ev->name);
                pending_at = now_ms() + 700;
            }

            p += sizeof(*ev) + ev->len;
        }
    }

    watch_user_folder(1);
}

/* milliseconds until something is due (-1: nothing) */
long long media_next(void) {

    if (!pending[0])
        return -1;

    long long left = pending_at - now_ms();

    return left > 0 ? left : 0;
}

void media_run(void) {

    if (!pending[0] || now_ms() < pending_at)
        return;

    open_files(pending);
    pending[0] = 0;
}
