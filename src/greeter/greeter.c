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
#include <sys/stat.h>
#include <sys/wait.h>
#include <math.h>
#include <linux/kd.h>

#include "../desktop/desktop.h"
#include "../libaccount.h"

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

/* Waits (at most 3 seconds) until the framebuffer text console is active. */
static void wait_for_text_console(void) {

    for (int tries = 0; tries < 60; tries++) {

        for (int i = 0; i < 4; i++) {

            char path[64], name[128] = "", bind[8] = "";

            snprintf(path, sizeof(path), "/sys/class/vtconsole/vtcon%d/name", i);

            FILE *f = fopen(path, "r");

            if (!f)
                continue;

            if (!fgets(name, sizeof(name), f))
                name[0] = 0;

            fclose(f);

            snprintf(path, sizeof(path), "/sys/class/vtconsole/vtcon%d/bind", i);
            f = fopen(path, "r");

            if (f) {

                if (!fgets(bind, sizeof(bind), f))
                    bind[0] = 0;

                fclose(f);
            }

            if (strstr(name, "frame buffer") && bind[0] == '1') {

                usleep(150 * 1000);     /* let it finish drawing */
                return;
            }
        }

        usleep(50 * 1000);
    }
}

/* If the greeter ever crashes, give the screen back to the text console. */
static void on_crash(int sig) {

    console_graphics(0);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* /etc/greeter.conf:  autologin=NAME */
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

/* 0 at the start, 1 at the end, slowing down smoothly at the end */
static double ease_out(long long t, long long duration) {

    double p = t >= duration ? 1.0 : t <= 0 ? 0.0 : (double)t / duration;

    return 1.0 - (1.0 - p) * (1.0 - p);
}

#define SPLASH_TEXT "Starting Bharat Linux"

static int splash_text_y(void) {

    return logo ? logo_y() + logo->h + 26 : screen_h / 2 + 20;
}

/*
 * One frame of the boot animation, t milliseconds after it started:
 *   0.0 - 0.9 s  the logo fades in
 *   0.6 - 1.2 s  the text fades in
 *   from 0.9 s   three dots (saffron, white, green) pulse in a wave
 * Only the parts that change are drawn and copied to the screen.
 */
static void splash_frame(long long t) {

    if (t <= 1000 && logo) {

        int x = logo_x(), y = logo_y();

        gfx_area(x, y, logo->w, logo->h);
        fill(x, y, logo->w, logo->h, BG_TOP);
        draw_image_alpha(logo, x, y, (int)(255 * ease_out(t, 900)));
        gfx_flush_rect(x, y, logo->w, logo->h);
    }

    int ty = splash_text_y();

    if (t >= 600 && t <= 1300) {

        int tw = text_width(&font_ui, SPLASH_TEXT);
        int tx = (screen_w - tw) / 2;

        gfx_area(tx, ty, tw, font_ui.height);
        fill(tx, ty, tw, font_ui.height, BG_TOP);
        text_alpha(&font_ui, tx, ty, SPLASH_TEXT, 0x8B949E, (int)(255 * ease_out(t - 600, 600)));
        gfx_flush_rect(tx, ty, tw, font_ui.height);
    }

    if (t >= 900) {

        static const uint32_t colors[3] = {C_SAFFRON, C_WHITE, C_GREEN};
        int dy = ty + font_ui.height + 22;
        int bx = screen_w / 2 - 40;

        gfx_area(bx, dy - 10, 80, 20);
        fill(bx, dy - 10, 80, 20, BG_TOP);

        for (int i = 0; i < 3; i++) {

            /* each dot grows and shrinks a little after the one before */
            double wave = (sin((t - 900) / 900.0 * 2 * M_PI - i * 0.9) + 1) / 2;
            int r = 3 + (int)(wave * 4 + 0.5);

            fill_circle(screen_w / 2 + (i - 1) * 24, dy, r, colors[i]);
        }

        gfx_flush_rect(bx, dy - 10, 80, 20);
    }

    gfx_area(0, 0, screen_w, screen_h);
}

/* The whole boot picture at moment t, copied to the screen in one go. */
static void splash_repaint(long long t) {

    gfx_area(0, 0, screen_w, screen_h);
    fill(0, 0, screen_w, screen_h, BG_TOP);

    if (logo)
        draw_image_alpha(logo, logo_x(), logo_y(), (int)(255 * ease_out(t, 900)));
    else
        center_text(&font_big, screen_h / 2 - 40, "Bharat Linux", C_WHITE);

    if (t > 600) {

        int tw = text_width(&font_ui, SPLASH_TEXT);

        text_alpha(&font_ui, (screen_w - tw) / 2, splash_text_y(), SPLASH_TEXT, 0x8B949E,
                   (int)(255 * ease_out(t - 600, 600)));
    }

    gfx_flush();
}

static void splash(void) {

    long long start = now_ms();
    long long last_repaint = -1000;

    /*
     * Show the animation for at least 3 seconds, then go on as soon as
     * the mouse driver is ready (at most 7 seconds in all).
     */
    while (1) {

        long long t = now_ms() - start;

        if ((t > 3000 && input_has_mouse()) || t > 7000)
            break;

        /*
         * Every half second, paint the whole picture again: if anything
         * else drew on the screen meanwhile, it does not stay visible.
         */
        if (t - last_repaint >= 500) {

            splash_repaint(t);
            last_repaint = t;
        }

        splash_frame(t);

        /* about 30 pictures a second; new devices are noticed here too */
        struct pollfd p = {.fd = input_watch_fd(), .events = POLLIN};

        if (poll(&p, 1, 33) > 0)
            input_watch_read();
    }
}

/* The logo glides up from the boot screen to its place on the login screen. */
static void glide_to_login(void) {

    if (!logo || screen_h < 700)
        return;

    int from = logo_y(), to = 14;
    long long start = now_ms();

    while (1) {

        long long t = now_ms() - start;
        double p = ease_out(t, 500);
        int y = from + (int)((to - from) * p);

        gfx_area(0, 0, screen_w, screen_h);

        /* the background changes from plain dark to the login colours */
        gradient(0, 0, screen_w, screen_h, BG_TOP,
                 (uint32_t)(((BG_BOTTOM >> 16 & 255) * p)) << 16 |
                 (uint32_t)(((BG_BOTTOM >> 8 & 255) * p)) << 8 |
                 (uint32_t)((BG_BOTTOM & 255) * p));
        draw_image(logo, logo_x(), y);
        gfx_flush();

        if (t >= 500)
            break;

        usleep(16 * 1000);
    }
}

/* ---------- forms: the login screen and the first-start setup ---------- */

struct box { int x, y, w, h; };

#define MAX_FIELDS 4

struct form {
    const char *title;
    const char *subtitle;           /* may be NULL */
    const char *button;
    int count;
    const char *labels[MAX_FIELDS];
    char *values[MAX_FIELDS];
    size_t sizes[MAX_FIELDS];
    int secret[MAX_FIELDS];
    int big_logo;                   /* the full logo above the card */
    void (*submit)(void);
};

static struct form *form;          /* the form on the screen now */
static int focus;                  /* which field gets the keys */

static int inside_box(struct box b) {

    return inside(mouse_x, mouse_y, b.x, b.y, b.w, b.h);
}

static int form_top(void) {

    return form->big_logo && logo && screen_h >= 700 ? 30 + logo->h : 140;
}

static struct box card_box(void) {

    int w = 360;
    int fields_y = (form->subtitle ? 82 : 58);
    int h = fields_y + form->count * 60 + 18 + 38 + 20 - 26;

    return (struct box){(screen_w - w) / 2, form_top(), w, h};
}

static struct box field_box(int i) {

    struct box c = card_box();

    return (struct box){c.x + 20, c.y + (form->subtitle ? 82 : 58) + i * 60, c.w - 40, 34};
}

static struct box submit_box(void) {

    struct box last = field_box(form->count - 1);

    return (struct box){last.x, last.y + last.h + 18, last.w, 38};
}

/* the buttons along the bottom of the screen */
static struct box text_mode_box(void) { return (struct box){20, screen_h - 54, 110, 34}; }
static struct box restart_box(void)   { return (struct box){screen_w - 250, screen_h - 54, 110, 34}; }
static struct box poweroff_box(void)  { return (struct box){screen_w - 130, screen_h - 54, 110, 34}; }

static void button(struct box b, const char *label, uint32_t fill_color, uint32_t text_color) {

    fill_round(b.x, b.y, b.w, b.h, 8, fill_color);

    int tw = text_width(&font_bold, label);

    text(&font_bold, b.x + (b.w - tw) / 2, b.y + (b.h - font_bold.height) / 2, label, text_color);
}

static void field(struct box b, const char *value, int secret, int focused) {

    fill_round(b.x, b.y, b.w, b.h, 7, focused ? C_SAFFRON : FIELD_LINE);
    fill_round(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 6, FIELD);

    int room = b.w - 28;
    int n = (int)strlen(value);
    int width = secret ? n * 12 : text_width(&font_ui, value);

    /* text longer than the box: slide it left so the end stays visible */
    int tx = b.x + 12 - (width > room ? width - room : 0);
    int ty = b.y + (b.h - font_ui.height) / 2;

    gfx_clip(b.x + 6, b.y, b.w - 12, b.h);

    if (secret) {

        /* one dot per letter: the password itself is never drawn */
        for (int i = 0; i < n; i++)
            fill_circle(tx + 4 + i * 12, b.y + b.h / 2, 4, C_WHITE);
    }
    else {

        text(&font_ui, tx, ty, value, C_WHITE);
    }

    tx += width;

    /* the text cursor */
    if (focused)
        fill(tx + 2, b.y + 8, 2, b.h - 16, C_SAFFRON);

    gfx_noclip();
}

static void draw_login(void) {

    gfx_area(0, 0, screen_w, screen_h);
    gradient(0, 0, screen_w, screen_h, BG_TOP, BG_BOTTOM);

    if (form->big_logo && logo && screen_h >= 700)
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

    /* the card with the form */
    struct box c = card_box();

    fill_round(c.x, c.y, c.w, c.h, 12, CARD);
    text(&font_bold, c.x + 20, c.y + 14, form->title, C_WHITE);

    if (form->subtitle)
        text(&font_ui, c.x + 20, c.y + 36, form->subtitle, 0x8B949E);

    for (int i = 0; i < form->count; i++) {

        struct box b = field_box(i);

        text(&font_ui, b.x, b.y - 20, form->labels[i], 0x8B949E);
        field(b, form->values[i], form->secret[i], focus == i);
    }

    button(submit_box(), form->button, C_SAFFRON, 0x1A1205);

    if (message[0])
        center_text(&font_ui, c.y + c.h + 14, message, 0xFF7B72);

    button(text_mode_box(), "Text mode", 0x1F2937, 0xC9D1D9);
    button(restart_box(), "Restart", 0x1F2937, 0xC9D1D9);
    button(poweroff_box(), "Power off", 0x7A1F1F, C_WHITE);

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

    /*
     * devd opens USB sticks for the person sitting at the computer:
     * tell it who that is (it watches this file).
     */
    FILE *who = fopen("/run/session-user.new", "w");

    if (who) {

        fprintf(who, "%s\n", user->name);
        fclose(who);
        rename("/run/session-user.new", "/run/session-user");
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

    /* logged out: devd closes this user's USB sticks */
    unlink("/run/session-user");

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

/* the user who logged in last is filled in next time */
#define LAST_USER_FILE "/var/lib/bharat/last-user"

static void save_last_user(const char *name) {

    mkdir("/var/lib/bharat", 0755);

    FILE *f = fopen(LAST_USER_FILE, "w");

    if (f) {

        fprintf(f, "%s\n", name);
        fclose(f);
    }
}

static void load_last_user(char *out, size_t size) {

    FILE *f = fopen(LAST_USER_FILE, "r");
    struct bharat_user u;

    out[0] = 0;

    if (f) {

        if (fgets(out, (int)size, f))
            out[strcspn(out, "\r\n")] = 0;

        fclose(f);
    }

    /* the user may have been deleted since */
    if (!out[0] || !find_user(out, 0, &u))
        first_user(out, size);
}

static void login_as(struct bharat_user *user) {

    message[0] = 0;
    save_last_user(user->name);
    session_ran = 1;
    after_session(run_session(user));
}

static void try_login(void) {

    struct bharat_user user;

    int ok = username[0] &&
             find_user(username, 0, &user) &&
             check_password(username, password);

    /* wipe the password from memory as soon as it is checked */
    memset(password, 0, sizeof(password));

    if (!ok) {

        auth_log("login: FAILED for %s (login screen)", username);
        snprintf(message, sizeof(message), "Wrong user name or password.");
        focus = 1;
        draw_login();

        /* the pause makes guessing passwords slow */
        sleep(2);
        return;
    }

    auth_log("login: %s logged in (login screen)", username);
    login_as(&user);
}

static struct form login_form = {
    "Sign in", NULL, "Log in", 2,
    {"User name", "Password"},
    {username, password},
    {sizeof(username), sizeof(password)},
    {0, 1},
    1, try_login
};

/* ---------- first start-up: create the administrator account ---------- */

static char new_name[64], new_pass[128], new_repeat[128], new_host[64] = "bharat-pc";

static void create_account(void) {

    const char *err = setup_first_account(new_name, new_pass, new_repeat, new_host);

    memset(new_pass, 0, sizeof(new_pass));
    memset(new_repeat, 0, sizeof(new_repeat));

    if (err) {

        snprintf(message, sizeof(message), "%s", err);
        focus = 0;
        return;
    }

    /* from now on: the normal login screen, and straight into the desktop */
    snprintf(username, sizeof(username), "%s", new_name);
    form = &login_form;
    focus = 1;

    struct bharat_user user;

    if (find_user(new_name, 0, &user)) {

        auth_log("login: %s logged in after creating the account", new_name);
        login_as(&user);
    }
}

static struct form setup_form = {
    "Welcome! Create your account",
    "It can manage this computer (administrator).",
    "Create account", 4,
    {"User name (small letters)", "Password (8 or more characters)", "Repeat the password", "Computer name"},
    {new_name, new_pass, new_repeat, new_host},
    {sizeof(new_name), sizeof(new_pass), sizeof(new_repeat), sizeof(new_host)},
    {0, 1, 1, 0},
    0, create_account
};

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

    /* Ctrl+Alt+F2: the text console, like on other Linux systems */
    if ((ev->mods & MOD_CTRL) && (ev->mods & MOD_ALT) && ev->key == K_F2)
        leave_to_text_mode();

    char *value = form->values[focus];
    size_t size = form->sizes[focus];
    size_t len = strlen(value);

    if (ev->key == '\t' || ev->key == K_DOWN) {

        focus = (focus + 1) % form->count;
    }
    else if (ev->key == K_UP) {

        focus = (focus + form->count - 1) % form->count;
    }
    else if (ev->key == '\r') {

        /* Enter: next box, or send the form from the last one */
        if (focus < form->count - 1)
            focus++;
        else
            form->submit();
    }
    else if (ev->key == '\b') {

        if (len > 0)
            value[len - 1] = 0;
    }
    else if (ev->key == 27) {

        for (int i = 0; i < form->count; i++)
            if (form->secret[i])
                memset(form->values[i], 0, form->sizes[i]);
    }
    else if (ev->key >= 32 && ev->key < 127 && len + 1 < size) {

        value[len] = (char)ev->key;
        value[len + 1] = 0;
    }
}

static void click(void) {

    for (int i = 0; i < form->count; i++) {

        if (inside_box(field_box(i))) {

            focus = i;
            return;
        }
    }

    if (inside_box(submit_box()))
        form->submit();
    else if (inside_box(restart_box()))
        power(EXIT_REBOOT);
    else if (inside_box(poweroff_box()))
        power(EXIT_POWEROFF);
    else if (inside_box(text_mode_box()))
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
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGBUS, on_crash);

    load_timezone();

    /*
     * Ubuntu's kernel starts the graphical text console late ("deferred
     * takeover"): only after the first text is printed, and then a moment
     * later in the background. If we draw before that moment, the
     * console's start-up wipes our picture: a black screen. So print a
     * line, then wait until the console has really started.
     * (run.sh also passes fbcon=nodefer, which starts it at boot.)
     */
    printf("\033[H\033[2JStarting Bharat Linux\n");
    fflush(stdout);
    wait_for_text_console();

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
        load_last_user(username, sizeof(username));

    form = &login_form;
    focus = username[0] ? 1 : 0;

    /* first start-up: nobody can log in yet, so create the account */
    if (first_boot()) {

        form = &setup_form;
        focus = 0;
        autologin[0] = 0;
    }

    /*
     * Automatic login happens once per start-up, not after logging out.
     * /run is emptied at every boot, so the marker file there is too.
     */
    struct bharat_user user;
    int first_time = access("/run/greeter-autologin-done", F_OK) != 0;

    if (autologin[0] && first_time) {

        close(open("/run/greeter-autologin-done", O_WRONLY | O_CREAT | O_CLOEXEC, 0644));


        if (find_user(autologin, 0, &user)) {

            session_ran = 1;
            after_session(run_session(&user));
        }
        else
            snprintf(message, sizeof(message), "Automatic login: no user %s.", autologin);
    }

    /* coming from the boot screen: a smooth change to the login screen */
    if (!session_ran && form == &login_form)
        glide_to_login();

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
