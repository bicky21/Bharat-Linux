/*
 * main.c - start the desktop, run the main loop, clean up at the end.
 *
 * The main loop sleeps in poll() until something happens: a key, the
 * mouse, output from a terminal, or the next full second (clock).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kd.h>

#include "desktop.h"

#define MAX_FDS 48

static int tty_fd = -1;
static volatile sig_atomic_t stop_now;

static void on_signal(int sig) {

    (void)sig;
    stop_now = 1;
}

/* Tell the text console to stop drawing on the screen (and back). */
static void console_graphics(int on) {

    if (tty_fd < 0)
        tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);

    if (tty_fd >= 0)
        ioctl(tty_fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
}

static void restore_console(void) {

    console_graphics(0);
}

/* If the desktop crashes, still give the user a working text console. */
static void on_crash(int sig) {

    restore_console();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void explain_screen_error(int err) {

    printf("desktop: cannot use the screen (/dev/fb0): %s\n", strerror(err));

    if (err == ENOENT)
        printf("There is no graphical screen. Start Bharat-linux with ./run.sh\n"
               "(not './run.sh text') so QEMU opens a window.\n");
    else if (err == EACCES)
        printf("Your user may not use the screen. Ask root to add you to\n"
               "the 'video' group in /etc/group, then log in again.\n");
}

int main(void) {

    int err = gfx_open();

    if (err < 0) {

        explain_screen_error(-err);
        return 1;
    }

    if (input_open() == 0) {

        gfx_close();
        printf("desktop: no keyboard or mouse found in /dev/input.\n"
               "Your user must be in the 'input' group (see /etc/group).\n");
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);

    console_graphics(1);

    wm_draw_wallpaper();
    wm_draw();

    long long next_tick = now_ms() + 1000;
    long long next_scan = now_ms() + 2000;

    while (!wm_quit && !stop_now) {

        struct pollfd pfd[MAX_FDS];
        int input[MAX_FDS];
        int win_fds[MAX_FDS];
        struct window *owners[MAX_FDS];

        int ni = input_fds(input, MAX_FDS / 2);
        int nw = wm_poll_fds(win_fds, owners, MAX_FDS / 2);

        for (int i = 0; i < ni; i++)
            pfd[i] = (struct pollfd){.fd = input[i], .events = POLLIN};

        for (int i = 0; i < nw; i++)
            pfd[ni + i] = (struct pollfd){.fd = win_fds[i], .events = POLLIN};

        long long wait = next_tick - now_ms();

        if (wait < 0)
            wait = 0;

        int ready = poll(pfd, ni + nw, (int)wait);

        if (ready < 0 && errno != EINTR)
            break;

        for (int i = 0; ready > 0 && i < ni; i++) {

            if (!(pfd[i].revents & POLLIN))
                continue;

            struct event events[64];
            int n = input_read(pfd[i].fd, events, 64);

            for (int k = 0; k < n; k++)
                wm_event(&events[k]);
        }

        for (int i = 0; ready > 0 && i < nw; i++) {

            if (pfd[ni + i].revents & (POLLIN | POLLHUP | POLLERR))
                owners[i]->readable(owners[i]);
        }

        wm_cleanup();

        long long now = now_ms();

        if (now >= next_tick) {

            wm_tick();
            next_tick = now + 1000;
        }

        /* a mouse plugged in (or allowed) later is picked up here */
        if (now >= next_scan) {

            input_rescan();
            next_scan = now + 3000;
        }

        if (wm_needs_redraw())
            wm_draw();
    }

    wm_close_all();
    input_close();
    gfx_close();
    restore_console();

    printf("Desktop closed.\n");

    return 0;
}
