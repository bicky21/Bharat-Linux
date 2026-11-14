/*
 * greeter - the boot screen and the graphical login screen.
 *
 * init starts it (as root) on the console when there is a graphics
 * screen. It:
 *   1. shows the Bharat Linux logo while the system starts (boot splash)
 *   2. logs a user in: automatically if /etc/greeter.conf says
 *      "autologin=NAME", otherwise with a name and password
 *   3. starts the desktop as that user, and waits for it to end
 *   4. acts on how the desktop ended: log out (back to step 2),
 *      restart, or power off
 *
 * Exit codes for init: 2 = the user asked for the text console,
 * 3 = no graphics screen (init falls back to the text login).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/kd.h>

#include "../desktop/desktop.h"
#include "../libuser.h"

#define BG_TOP    0x04060B          /* the logo's own background colour */
#define BG_BOTTOM 0x0E1B30
#define CARD      0x121C2E
#define FIELD     0x0A1120
#define FIELD_LINE 0x2A3A55

#define EXIT_TEXT_MODE 2
#define EXIT_NO_SCREEN 3

static struct image *logo, *badge;

static char username[64];
static char password[128];
static int focus = 1;                /* 0 = user name box, 1 = password box */
static char message[128];
static int session_ran;              /* the devices were closed and opened again */

/* ---------- small helpers ---------- */

long long now_ms(void) {

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int inside(int px, int py, int x, int y, int w, int h) {

    return px >= x && py >= y && px < x + w && py < y + h;
}

static void console_graphics(int on) {

    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);

    if (fd >= 0) {

        ioctl(fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
        close(fd);
    }
}

/* /etc/greeter.conf:  autologin=bicky */
static void read_config(char *autologin, size_t size) {

    autologin[0] = 0;

    FILE *f = fopen("/etc/greeter.conf", "r");

    if (!f)
        return;

    char line[128];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\r\n")] = 0;

        if (strncmp(line, "autologin=", 10) == 0)
            snprintf(autologin, size, "%.63s", line + 10);
    }

    fclose(f);
}

/* The first normal user (uid 1000 or more), to fill in the name box. */
static void first_user(char *out, size_t size) {

    FILE *f = fopen("/etc/passwd", "r");
    char line[512];
    struct bharat_user u;

    out[0] = 0;

    while (f && fgets(line, sizeof(line), f)) {

        if (parse_passwd_line(line, &u) && u.uid >= 1000) {

            snprintf(out, size, "%s", u.name);
            break;
        }
    }

    if (f)
        fclose(f);
}

static void center_text(struct font *f, int y, const char *s, uint32_t c) {

    text(f, (screen_w - text_width(f, s)) / 2, y, s, c);
}

/* ---------- boot splash ---------- */

static int logo_x(void) { return logo ? (screen_w - logo->w) / 2 : 0; }
static int logo_y(void) { return logo ? screen_h / 2 - logo->h / 2 - 60 : 0; }

static void draw_splash_bar(long long t) {

    int bw = 220, bx = (screen_w - bw) / 2;
    int by = logo ? logo_y() + logo->h + 30 : screen_h / 2 + 40;

    gfx_area(bx, by, bw, 4);
    fill(bx, by, bw, 4, 0x1A2333);

    /* a short saffron piece that slides from left to right */
    int seg = 60;
    int pos = (int)(t % 1400) * (bw + seg) / 1400 - seg;

    gfx_clip(bx, by, bw, 4);
    fill(bx + pos, by, seg, 4, C_SAFFRON);
    gfx_flush_rect(bx, by, bw, 4);
    gfx_area(0, 0, screen_w, screen_h);
}

static void splash(void) {

    gfx_area(0, 0, screen_w, screen_h);
    fill(0, 0, screen_w, screen_h, BG_TOP);

    if (logo) {

        draw_image(logo, logo_x(), logo_y());
        center_text(&font_ui, logo_y() + logo->h + 48, "Starting Bharat Linux", 0x8B949E);
    }
    else {

        center_text(&font_big, screen_h / 2 - 20, "Bharat Linux", C_WHITE);
    }

    gfx_flush();

    long long start = now_ms();

    /*
     * Wait at least 1.5 seconds (so the logo is seen), until the mouse
     * driver is ready, but never more than 6 seconds.
     */
    while (1) {

        long long t = now_ms() - start;

        if ((t > 1500 && input_has_mouse()) || t > 6000)
            break;

        draw_splash_bar(t);

        /* new devices (the mouse driver loading) are noticed here */
        struct pollfd p = {.fd = input_watch_fd(), .events = POLLIN};

        if (poll(&p, 1, 33) > 0)
            input_watch_read();
    }
}

/* ---------- the login screen ---------- */

struct box { int x, y, w, h; };

struct layout {
    struct box card, user, pass, login, text_mode, restart, poweroff;
    int top;              /* where the card starts */
};

static struct layout layout(void) {

    struct layout l;
    int cw = 340;

    /* the logo on top if there is room, otherwise the small badge */
    int top = logo && screen_h >= 700 ? 26 + logo->h : 150;

    l.top = top;
    l.card = (struct box){(screen_w - cw) / 2, top + 4, cw, 226};

    int fx = l.card.x + 20, fw = cw - 40;

    l.user  = (struct box){fx, l.card.y + 58, fw, 34};
    l.pass  = (struct box){fx, l.card.y + 118, fw, 34};
    l.login = (struct box){fx, l.card.y + 170, fw, 38};

    int by = screen_h - 54;

    l.text_mode = (struct box){20, by, 110, 34};
    l.poweroff  = (struct box){screen_w - 20 - 110, by, 110, 34};
    l.restart   = (struct box){screen_w - 20 - 230, by, 110, 34};

    return l;
}

static void button(struct box b, const char *label, uint32_t fill_color, uint32_t text_color) {

    fill_round(b.x, b.y, b.w, b.h, 8, fill_color);

    int tw = text_width(&font_bold, label);

    text(&font_bold, b.x + (b.w - tw) / 2, b.y + (b.h - font_bold.height) / 2, label, text_color);
}

static void field(struct box b, const char *value, int secret, int focused) {

    fill_round(b.x, b.y, b.w, b.h, 7, focused ? C_SAFFRON : FIELD_LINE);
    fill_round(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 6, FIELD);

    int tx = b.x + 12, ty = b.y + (b.h - font_ui.height) / 2;

    if (secret) {

        /* one dot per letter: the password itself is never drawn */
        int n = (int)strlen(value);

        for (int i = 0; i < n && 12 + i * 12 < b.w - 20; i++)
            fill_circle(tx + 4 + i * 12, b.y + b.h / 2, 4, C_WHITE);

        tx += n * 12;
    }
    else {

        tx += text(&font_ui, tx, ty, value, C_WHITE);
    }

    /* the text cursor */
    if (focused)
        fill(tx + 2, b.y + 8, 2, b.h - 16, C_SAFFRON);
}

static void draw_login(void) {

    struct layout l = layout();

    gfx_area(0, 0, screen_w, screen_h);
    gradient(0, 0, screen_w, screen_h, BG_TOP, BG_BOTTOM);

    if (logo && screen_h >= 700)
        draw_image(logo, logo_x(), 14);
    else if (badge)
        draw_image(badge, (screen_w - badge->w) / 2, 30);

    /* computer name, top left; clock, top right */
    char host[64] = "bharat";

    gethostname(host, sizeof(host) - 1);
    text(&font_bold, 20, 16, host, 0xC9D1D9);

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char clock_text[32];

    strftime(clock_text, sizeof(clock_text), "%a %d %b  %H:%M", tm);
    text(&font_bold, screen_w - 20 - text_width(&font_bold, clock_text), 16, clock_text, 0xC9D1D9);

    /* the card */
    fill_round(l.card.x, l.card.y, l.card.w, l.card.h, 12, CARD);
    text(&font_bold, l.card.x + 20, l.card.y + 14, "Sign in", C_WHITE);

    text(&font_ui, l.user.x, l.user.y - 20, "User name", 0x8B949E);
    field(l.user, username, 0, focus == 0);

    text(&font_ui, l.pass.x, l.pass.y - 20, "Password", 0x8B949E);
    field(l.pass, password, 1, focus == 1);

    button(l.login, "Log in", C_SAFFRON, 0x1A1205);

    if (message[0])
        center_text(&font_ui, l.card.y + l.card.h + 14, message, 0xFF7B72);

    button(l.text_mode, "Text mode", 0x1F2937, 0xC9D1D9);
    button(l.restart, "Restart", 0x1F2937, 0xC9D1D9);
    button(l.poweroff, "Power off", 0x7A1F1F, C_WHITE);

    /* remember the picture without the pointer, to move it cheaply */
    save_background();

    draw_cursor(mouse_x, mouse_y);
    gfx_flush();
}

/* Move only the pointer: repaint the old spot, draw it at the new one. */
static void move_cursor(int old_x, int old_y) {

    gfx_area(old_x, old_y, CURSOR_W, CURSOR_H);
    restore_background();
    gfx_flush_rect(old_x, old_y, CURSOR_W, CURSOR_H);

    gfx_area(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
    restore_background();
    draw_cursor(mouse_x, mouse_y);
    gfx_flush_rect(mouse_x, mouse_y, CURSOR_W, CURSOR_H);

    gfx_area(0, 0, screen_w, screen_h);
}

static void show_message_screen(const char *s) {

    gfx_area(0, 0, screen_w, screen_h);
    fill(0, 0, screen_w, screen_h, BG_TOP);

    if (logo)
        draw_image(logo, logo_x(), logo_y());

    center_text(&font_ui, logo ? logo_y() + logo->h + 40 : screen_h / 2, s, 0x8B949E);
    gfx_flush();
}

/* ---------- starting the desktop ---------- */

/* Runs the desktop as this user and returns how it ended (EXIT_...). */
static int run_session(struct bharat_user *user) {

    show_message_screen("Starting the desktop");

    /* lend the keyboard and mouse to the desktop */
    input_release();

    /* the user owns the console while logged in */
    if (fchown(STDIN_FILENO, user->uid, user->gid) != 0) {
        /* not fatal */
    }

    pid_t pid = fork();

    if (pid == 0) {

        signal(SIGINT, SIG_DFL);

        if (become_user(user) != 0)
            _exit(126);

        setenv("BHARAT_SESSION", "1", 1);

        execl("/bin/desktop", "desktop", NULL);

        _exit(127);
    }

    int status = 0;

    if (pid > 0) {

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            /* try again */
        }
    }

    if (fchown(STDIN_FILENO, 0, 0) != 0) {
        /* not fatal */
    }

    /* the desktop gave the screen back: it is ours again */
    console_graphics(1);
    input_take();

    if (pid < 0 || !WIFEXITED(status)) {

        snprintf(message, sizeof(message), "The desktop stopped unexpectedly.");
        return EXIT_LOGOUT;
    }

    int code = WEXITSTATUS(status);

    if (code == 126 || code == 127) {

        snprintf(message, sizeof(message), "Could not start the desktop for %s.", user->name);
        return EXIT_LOGOUT;
    }

    return code;
}

static void power(int code) {

    show_message_screen(code == EXIT_REBOOT ? "Restarting" : "Powering off");

    input_close();

    /* these programs save everything to disk and then switch off */
    if (code == EXIT_REBOOT)
        execl("/bin/reboot", "reboot", NULL);
    else
        execl("/bin/poweroff", "poweroff", NULL);

    input_open();
    snprintf(message, sizeof(message), "Could not power off.");
}

static void after_session(int code) {

    if (code == EXIT_POWEROFF || code == EXIT_REBOOT)
        power(code);
}

static void try_login(void) {

    struct bharat_user user;

    int ok = username[0] &&
             find_user(username, 0, &user) &&
             check_password(username, password);

    /* wipe the password from memory as soon as it is checked */
    memset(password, 0, sizeof(password));

    if (!ok) {

        snprintf(message, sizeof(message), "Wrong user name or password.");
        focus = 1;
        draw_login();

        /* the pause makes guessing passwords slow */
        sleep(2);
        return;
    }

    message[0] = 0;
    session_ran = 1;
    after_session(run_session(&user));
}

static void leave_to_text_mode(void) {

    input_close();
    gfx_close();
    console_graphics(0);

    /* a clean text screen for the text login */
    printf("\033[H\033[2J");
    fflush(stdout);

    exit(EXIT_TEXT_MODE);
}

static void key_press(struct event *ev) {

    char *value = focus == 0 ? username : password;
    size_t size = focus == 0 ? sizeof(username) : sizeof(password);
    size_t len = strlen(value);

    if (ev->key == '\t' || ev->key == K_UP || ev->key == K_DOWN) {

        focus = !focus;
    }
    else if (ev->key == '\r') {

        if (focus == 0)
            focus = 1;
        else
            try_login();
    }
    else if (ev->key == '\b') {

        if (len > 0)
            value[len - 1] = 0;
    }
    else if (ev->key == 27) {

        memset(password, 0, sizeof(password));
    }
    else if (ev->key >= 32 && ev->key < 127 && len + 1 < size) {

        value[len] = (char)ev->key;
        value[len + 1] = 0;
    }
}

static void click(void) {

    struct layout l = layout();

    if (inside(mouse_x, mouse_y, l.user.x, l.user.y, l.user.w, l.user.h))
        focus = 0;
    else if (inside(mouse_x, mouse_y, l.pass.x, l.pass.y, l.pass.w, l.pass.h))
        focus = 1;
    else if (inside(mouse_x, mouse_y, l.login.x, l.login.y, l.login.w, l.login.h))
        try_login();
    else if (inside(mouse_x, mouse_y, l.restart.x, l.restart.y, l.restart.w, l.restart.h))
        power(EXIT_REBOOT);
    else if (inside(mouse_x, mouse_y, l.poweroff.x, l.poweroff.y, l.poweroff.w, l.poweroff.h))
        power(EXIT_POWEROFF);
    else if (inside(mouse_x, mouse_y, l.text_mode.x, l.text_mode.y, l.text_mode.w, l.text_mode.h))
        leave_to_text_mode();
}

/* ---------- main ---------- */

int main(void) {

    if (getuid() != 0) {

        printf("greeter is started by init, as root.\n");
        return 1;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    load_timezone();

    /*
     * Write a line to the text console before opening the screen.
     * We found that if nothing has been written to the console since
     * boot, our pictures never reach the display (it stays black).
     */
    printf("\033[H\033[2JStarting Bharat Linux\n");
    fflush(stdout);

    if (gfx_open() < 0)
        return EXIT_NO_SCREEN;

    console_graphics(1);

    logo = image_load("/usr/share/bharat/logo.img");
    badge = image_load("/usr/share/bharat/mark96.img");

    input_open();

    splash();

    char autologin[64];

    read_config(autologin, sizeof(autologin));

    if (autologin[0])
        snprintf(username, sizeof(username), "%s", autologin);
    else
        first_user(username, sizeof(username));

    focus = username[0] ? 1 : 0;

    /*
     * Automatic login happens once per start-up, not after logging out.
     * /run is emptied at every boot, so the marker file there is too.
     */
    struct bharat_user user;
    int first_time = access("/run/greeter-autologin-done", F_OK) != 0;

    if (autologin[0] && first_time) {

        close(open("/run/greeter-autologin-done", O_WRONLY | O_CREAT | O_CLOEXEC, 0644));


        if (find_user(autologin, 0, &user))
            after_session(run_session(&user));
        else
            snprintf(message, sizeof(message), "Automatic login: no user %s.", autologin);
    }

    draw_login();

    while (1) {

        struct pollfd p[20];
        int fds[18];
        int n = input_fds(fds, 18);

        for (int i = 0; i < n; i++)
            p[i] = (struct pollfd){.fd = fds[i], .events = POLLIN};

        p[n] = (struct pollfd){.fd = input_watch_fd(), .events = POLLIN};

        /* wake up for the next minute, so the clock stays right */
        int wait = (int)(60 - time(NULL) % 60) * 1000 + 20;

        int ready = poll(p, n + 1, wait);

        if (ready == 0) {

            draw_login();
            continue;
        }

        if (p[n].revents & POLLIN)
            input_watch_read();

        session_ran = 0;

        for (int i = 0; i < n && !session_ran; i++) {

            if (!(p[i].revents & POLLIN))
                continue;

            struct event ev[64];
            int old_x = mouse_x, old_y = mouse_y;
            int count = input_read(p[i].fd, ev, 64);
            int redraw = 0, moved = 0;

            for (int k = 0; k < count; k++) {

                if (ev[k].type == EV_MOUSE_MOVE) {

                    moved = 1;
                }
                else if (ev[k].type == EV_MOUSE_DOWN && ev[k].button == 1) {

                    click();
                    redraw = 1;
                }
                else if (ev[k].type == EV_KEY_PRESS) {

                    key_press(&ev[k]);
                    redraw = 1;
                }
            }

            if (redraw || session_ran)
                draw_login();
            else if (moved)
                move_cursor(old_x, old_y);
        }
    }
}
