/*
 * input.c - keyboard and mouse, read straight from the kernel.
 *
 * Every input device has a file /dev/input/eventN. Reading it gives
 * "struct input_event" records: a key went down, the mouse moved 3
 * pixels left, and so on. We "grab" each device so the text console
 * underneath does not also receive the keys.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <linux/input.h>

#include "desktop.h"

#define MAX_DEVICES 16

struct device {
    int fd;
    char name[64];        /* eventN */
    int is_tablet;        /* gives absolute positions (QEMU tablet) */
    struct input_absinfo ax, ay;
};

static struct device devices[MAX_DEVICES];
static int device_count;
static int mouse_count;

int mouse_x, mouse_y;

static int shift, ctrl, alt, caps;

/* the kernel tells us through this file when /dev/input changes */
static int watch_fd = -1;

/* US keyboard layout: key code -> character, without and with Shift */
static const char normal_map[128] =
    "\0\033" "1234567890-=" "\b\t"
    "qwertyuiop[]" "\r\0"
    "asdfghjkl;'`" "\0\\"
    "zxcvbnm,./" "\0*\0 ";

static const char shift_map[128] =
    "\0\033" "!@#$%^&*()_+" "\b\t"
    "QWERTYUIOP{}" "\r\0"
    "ASDFGHJKL:\"~" "\0|"
    "ZXCVBNM<>?" "\0*\0 ";

#define BIT(array, n) ((array[(n) / 8] >> ((n) % 8)) & 1)

static int already_open(const char *name) {

    for (int i = 0; i < device_count; i++) {

        if (strcmp(devices[i].name, name) == 0)
            return 1;
    }

    return 0;
}

static void open_device(const char *name) {

    if (device_count >= MAX_DEVICES || already_open(name))
        return;

    char path[300];

    snprintf(path, sizeof(path), "/dev/input/%.256s", name);

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0)
        return;

    unsigned char keys[KEY_MAX / 8 + 1] = {0};
    unsigned char rel[REL_MAX / 8 + 1] = {0};
    unsigned char abs_bits[ABS_MAX / 8 + 1] = {0};

    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);

    int keyboard = BIT(keys, KEY_A) && BIT(keys, KEY_ENTER);
    int mouse = BIT(rel, REL_X) && BIT(keys, BTN_LEFT);
    int tablet = BIT(abs_bits, ABS_X) && BIT(keys, BTN_LEFT);

    if (!keyboard && !mouse && !tablet) {

        close(fd);
        return;
    }

    struct device *d = &devices[device_count++];

    memset(d, 0, sizeof(*d));
    d->fd = fd;
    snprintf(d->name, sizeof(d->name), "%.60s", name);

    if (tablet && !mouse) {

        d->is_tablet = 1;
        ioctl(fd, EVIOCGABS(ABS_X), &d->ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &d->ay);
    }

    if (mouse || tablet)
        mouse_count++;

    /* take the device for ourselves only */
    ioctl(fd, EVIOCGRAB, 1);
}

void input_rescan(void) {

    DIR *dir = opendir("/dev/input");

    if (!dir)
        return;

    struct dirent *e;

    while ((e = readdir(dir)) != NULL) {

        if (strncmp(e->d_name, "event", 5) == 0)
            open_device(e->d_name);
    }

    closedir(dir);
}

int input_open(void) {

    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    /*
     * New device, or a device that just became ours (devd changed its
     * group): look again. Without this we would have to check every few
     * seconds, which wakes the computer up for nothing.
     */
    watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    if (watch_fd >= 0)
        inotify_add_watch(watch_fd, "/dev/input", IN_CREATE | IN_ATTRIB);

    input_rescan();

    return device_count;
}

int input_watch_fd(void) {

    return watch_fd;
}

void input_watch_read(void) {

    char buf[4096];

    while (read(watch_fd, buf, sizeof(buf)) > 0) {
        /* we only need to know that something changed */
    }

    input_rescan();
}

/*
 * Lend the keyboard and mouse to another program (the desktop) without
 * closing them. Closing would make the USB tablet look unused for a
 * moment, and QEMU would then switch the pointer to the PS/2 mouse.
 */
void input_release(void) {

    for (int i = 0; i < device_count; i++)
        ioctl(devices[i].fd, EVIOCGRAB, 0);
}

/* Take them back, and throw away everything typed meanwhile. */
void input_take(void) {

    struct input_event junk[64];

    for (int i = 0; i < device_count; i++) {

        while (read(devices[i].fd, junk, sizeof(junk)) > 0) {
            /* old events: the desktop already handled them */
        }

        ioctl(devices[i].fd, EVIOCGRAB, 1);
    }

    shift = ctrl = alt = 0;

    input_rescan();
}

void input_close(void) {

    if (watch_fd >= 0)
        close(watch_fd);

    watch_fd = -1;
    mouse_count = 0;

    for (int i = 0; i < device_count; i++) {

        ioctl(devices[i].fd, EVIOCGRAB, 0);
        close(devices[i].fd);
    }

    device_count = 0;
}

int input_has_mouse(void) {

    return mouse_count > 0;
}

int input_fds(int *fds, int max) {

    int n = 0;

    for (int i = 0; i < device_count && n < max; i++)
        fds[n++] = devices[i].fd;

    return n;
}

static int mods(void) {

    return (shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0);
}

/* Turn a key code into a character or K_... value (0 = ignore). */
static int translate(int code) {

    switch (code) {

        case KEY_UP:        return K_UP;
        case KEY_DOWN:      return K_DOWN;
        case KEY_LEFT:      return K_LEFT;
        case KEY_RIGHT:     return K_RIGHT;
        case KEY_HOME:      return K_HOME;
        case KEY_END:       return K_END;
        case KEY_PAGEUP:    return K_PGUP;
        case KEY_PAGEDOWN:  return K_PGDN;
        case KEY_DELETE:    return K_DELETE;
        case KEY_F4:        return K_F4;
        case KEY_F2:        return K_F2;
        case KEY_KPENTER:   return '\r';
        case KEY_BACKSPACE: return '\b';
    }

    if (code <= 0 || code >= 128)
        return 0;

    int c = shift ? shift_map[code] : normal_map[code];

    /* Caps Lock only changes letters */
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && c >= 'A' && c <= 'Z') c += 32;

    return (unsigned char)c;
}

static struct device *find(int fd) {

    for (int i = 0; i < device_count; i++) {

        if (devices[i].fd == fd)
            return &devices[i];
    }

    return NULL;
}

static void clamp_mouse(void) {

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
}

/* Reads what is waiting on one device and turns it into our events. */
int input_read(int fd, struct event *out, int max) {

    struct device *d = find(fd);
    struct input_event raw[64];
    int n = 0;

    if (!d)
        return 0;

    ssize_t got = read(fd, raw, sizeof(raw));

    if (got <= 0)
        return 0;

    int moved = 0;
    int old_x = mouse_x, old_y = mouse_y;

    for (int i = 0; i < got / (int)sizeof(raw[0]) && n < max - 1; i++) {

        struct input_event *e = &raw[i];

        if (e->type == EV_REL) {

            if (e->code == REL_X) { mouse_x += e->value; moved = 1; }
            if (e->code == REL_Y) { mouse_y += e->value; moved = 1; }

            if (e->code == REL_WHEEL)
                out[n++] = (struct event){.type = EV_WHEEL, .value = e->value};
        }
        else if (e->type == EV_ABS && d->is_tablet) {

            /* scale the tablet's numbers to screen pixels */
            if (e->code == ABS_X && d->ax.maximum > d->ax.minimum) {

                mouse_x = (int)((long long)(e->value - d->ax.minimum) * screen_w /
                                (d->ax.maximum - d->ax.minimum + 1));
                moved = 1;
            }

            if (e->code == ABS_Y && d->ay.maximum > d->ay.minimum) {

                mouse_y = (int)((long long)(e->value - d->ay.minimum) * screen_h /
                                (d->ay.maximum - d->ay.minimum + 1));
                moved = 1;
            }
        }
        else if (e->type == EV_KEY) {

            int code = e->code;
            int down = e->value != 0;     /* 1 = press, 2 = repeat */

            if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE) {

                if (e->value == 2)
                    continue;

                int button = code == BTN_LEFT ? 1 : code == BTN_RIGHT ? 2 : 3;

                out[n++] = (struct event){
                    .type = down ? EV_MOUSE_DOWN : EV_MOUSE_UP,
                    .button = button,
                    .mods = mods()
                };

                continue;
            }

            if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { shift = down; continue; }
            if (code == KEY_LEFTCTRL  || code == KEY_RIGHTCTRL)  { ctrl = down;  continue; }
            if (code == KEY_LEFTALT   || code == KEY_RIGHTALT)   { alt = down;   continue; }

            if (code == KEY_CAPSLOCK) {

                if (e->value == 1)
                    caps = !caps;

                continue;
            }

            if (!down)
                continue;

            int key = translate(code);

            if (key)
                out[n++] = (struct event){.type = EV_KEY_PRESS, .key = key, .mods = mods()};
        }
    }

    if (moved) {

        clamp_mouse();

        /* value = where the pointer was before (x in the high half) */
        if (n < max)
            out[n++] = (struct event){.type = EV_MOUSE_MOVE,
                                      .value = (old_x << 16) | old_y};
    }

    return n;
}
