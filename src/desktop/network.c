/*
 * network.c - the network icon in the taskbar and the Network app.
 *
 * netd (the network service) writes how things stand into
 * /run/network/state; we watch that folder and show it. Buttons ask
 * netd through /run/netd.sock (it lets the person at the desktop
 * switch the network on and off, without sudo).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "desktop.h"

#define MAX_CARDS 8

struct card {
    char name[32], state[32], addr[40], gateway[20], dns[20], how[16];
};

static struct card cards[MAX_CARDS];
static int ncards;
static int watch_fd = -1, watched;
static char summary[96];

static void load(void) {

    FILE *f = fopen("/run/network/state", "r");
    char line[256];

    ncards = 0;

    while (f && ncards < MAX_CARDS && fgets(line, sizeof(line), f)) {

        struct card *c = &cards[ncards];

        if (sscanf(line, "%31s %31s %39s %19s %19s %15s", c->name, c->state, c->addr,
                   c->gateway, c->dns, c->how) == 6)
            ncards++;
    }

    if (f)
        fclose(f);

    summary[0] = 0;

    for (int i = 0; i < ncards; i++) {

        if (!strcmp(cards[i].state, "connected")) {

            snprintf(summary, sizeof(summary), "%s  %s", cards[i].name, cards[i].addr);
            return;
        }
    }

    snprintf(summary, sizeof(summary), "%s", ncards ? "Not connected" : "No network card");
}

int net_connected(void) {

    static int loaded;

    if (!loaded) {

        loaded = 1;
        net_watch_fd();
        load();
    }

    if (!ncards)
        return -1;

    for (int i = 0; i < ncards; i++)
        if (!strcmp(cards[i].state, "connected"))
            return 1;

    return 0;
}

const char *net_summary(void) {

    return summary;
}

int net_watch_fd(void) {

    if (watch_fd < 0) {

        watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

        /* netd makes /run/network soon after start; until then watch /run */
        if (watch_fd >= 0 && inotify_add_watch(watch_fd, "/run/network", IN_MOVED_TO | IN_CLOSE_WRITE) >= 0)
            watched = 1;
        else if (watch_fd >= 0)
            inotify_add_watch(watch_fd, "/run", IN_CREATE | IN_ONLYDIR);
    }

    return watch_fd;
}

void net_watch_read(void) {

    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (read(watch_fd, buf, sizeof(buf)) > 0)
        ;

    if (!watched && inotify_add_watch(watch_fd, "/run/network", IN_MOVED_TO | IN_CLOSE_WRITE) >= 0)
        watched = 1;

    load();

    /* the taskbar icon, and an open Network window, show it */
    wm_damage(screen_w - 200, screen_h - TASKBAR_H, 200, TASKBAR_H);
}

/* ---------- asking netd ---------- */

static void ask(const char *request, char *reply, size_t size) {

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};

    snprintf(a.sun_path, sizeof(a.sun_path), "/run/netd.sock");
    snprintf(reply, size, "error The network service is not running.");

    if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        write(s, request, strlen(request)) > 0) {

        ssize_t n = read(s, reply, size - 1);

        reply[n > 0 ? n : 0] = 0;
    }

    if (s >= 0)
        close(s);

    reply[strcspn(reply, "\n")] = 0;
}

/* ---------- the Network app ---------- */

struct netapp {
    char message[160];
    int ok;
};

#define CARD_H 118
#define BTN_W 110

static void netapp_draw(struct window *win, int x, int y, int w, int h) {

    struct netapp *a = win->data;

    fill(x, y, w, h, C_WHITE);

    if (!ncards)
        text(&font_ui, x + 18, y + 18, "No wired network card was found. (Wi-Fi is not supported yet.)", C_MUTED);

    for (int i = 0; i < ncards; i++) {

        struct card *c = &cards[i];
        int cy = y + 12 + i * CARD_H;
        int on = !strcmp(c->state, "connected");

        fill_round(x + 12, cy, w - 24, CARD_H - 12, 8, C_PANEL);
        frame(x + 12, cy, w - 24, CARD_H - 12, C_BORDER);

        icon_network(x + 26, cy + 14, 28, on ? 1 : 0);

        char head[96];

        snprintf(head, sizeof(head), "Wired (%s)", c->name);
        text(&font_bold, x + 66, cy + 12, head, C_TEXT);

        const char *how = on ? "Connected" :
                          !strcmp(c->state, "no-cable") ? "Cable unplugged" :
                          !strcmp(c->state, "off") ? "Switched off" :
                          !strcmp(c->state, "no-answer") ? "No answer from the network (trying again)" :
                          "Connecting...";

        text(&font_ui, x + 66, cy + 32, how, on ? C_GREEN : C_MUTED);

        if (on) {

            char line[160];

            snprintf(line, sizeof(line), "Address %s    Gateway %s    DNS %s    (%s)",
                     c->addr, c->gateway, c->dns, !strcmp(c->how, "static") ? "fixed" : "automatic");
            text(&font_ui, x + 26, cy + 62, line, C_TEXT);
        }

        /* buttons: Renew, Turn off / Turn on */
        int bx = x + w - 24 - 2 * BTN_W - 10;

        fill_round(bx, cy + 12, BTN_W, 28, 6, C_WHITE);
        frame(bx, cy + 12, BTN_W, 28, C_BORDER);
        text(&font_ui, bx + (BTN_W - text_width(&font_ui, "Reconnect")) / 2, cy + 18, "Reconnect", C_TEXT);

        const char *t = strcmp(c->state, "off") ? "Turn off" : "Turn on";

        fill_round(bx + BTN_W + 10, cy + 12, BTN_W, 28, 6, strcmp(c->state, "off") ? 0xCF222E : C_SAFFRON);
        text(&font_bold, bx + BTN_W + 10 + (BTN_W - text_width(&font_bold, t)) / 2, cy + 18, t, C_WHITE);
    }

    if (a->message[0])
        text(&font_ui, x + 18, y + h - 28, a->message, a->ok ? C_GREEN : 0xCF222E);
    else
        text(&font_ui, x + 18, y + h - 28, "Fixed settings: /etc/network.conf. Commands: network, ip a, ping.", C_MUTED);
}

static void netapp_click(struct window *win, int x, int y, int button, int dbl) {

    struct netapp *a = win->data;
    int cx, cy, w, h;

    (void)dbl;

    if (button != 1)
        return;

    content_box(win, &cx, &cy, &w, &h);

    int i = (y - 12) / CARD_H;

    if (i < 0 || i >= ncards || (y - 12) % CARD_H < 12 || (y - 12) % CARD_H > 40)
        return;

    int bx = w - 24 - 2 * BTN_W - 10;
    char req[64], reply[200];

    if (x >= bx && x < bx + BTN_W)
        snprintf(req, sizeof(req), "renew %s\n", cards[i].name);
    else if (x >= bx + BTN_W + 10 && x < bx + 2 * BTN_W + 10)
        snprintf(req, sizeof(req), "%s %s\n", strcmp(cards[i].state, "off") ? "off" : "on", cards[i].name);
    else
        return;

    ask(req, reply, sizeof(reply));
    a->ok = !strncmp(reply, "ok", 2);
    snprintf(a->message, sizeof(a->message), "%.150s", a->ok ? (reply[2] ? reply + 3 : "Done") : reply + 6);
}

static void netapp_tick(struct window *win) {

    (void)win;
    load();
}

static void netapp_destroy(struct window *win) {

    free(win->data);
}

void open_network(void) {

    load();

    struct window *win = wm_new_window("Network", 620, TITLE_H + 40 + (ncards ? ncards : 1) * CARD_H + 30);

    if (!win)
        return;

    win->data = calloc(1, sizeof(struct netapp));
    win->draw = netapp_draw;
    win->click = netapp_click;
    win->tick = netapp_tick;
    win->destroy = netapp_destroy;
    win->min_w = 560;
}
