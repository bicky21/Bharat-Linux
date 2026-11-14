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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
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

/*
 * When to wake up next if nothing happens: in one second when a window
 * shows live numbers (System Monitor), otherwise when the clock in the
 * taskbar has to change (the next full minute).
 */
static long long next_wakeup(void) {

    if (wm_wants_seconds())
        return now_ms() + 1000;

    return now_ms() + (60 - time(NULL) % 60) * 1000 + 20;
}

/*
 * Started from the login screen, the screen stays in graphics mode when
 * we exit, because the login screen comes straight back. Started from a
 * shell, give the text console back.
 */
static void restore_console(void) {

    if (!getenv("BHARAT_SESSION"))
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

/*
 * Only one desktop may run at a time: a second one would fight the first
 * for the screen, keyboard and mouse. Two checks:
 *   1. BHARAT_DESKTOP is set in every Terminal the desktop opens.
 *   2. A lock on a file in /tmp, held while the desktop runs.
 */
static int already_running(void) {

    if (getenv("BHARAT_DESKTOP"))
        return 1;

    int fd = open("/tmp/.bharat-desktop.lock", O_RDONLY | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0)
        fd = open("/tmp/.bharat-desktop.lock", O_RDONLY | O_CLOEXEC);

    /* the lock stays ours until this program ends (fd is never closed) */
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0;
}

int main(void) {

    if (already_running()) {

        printf("The desktop is already running.\n"
               "This Terminal is inside it: just keep working here.\n");
        return 1;
    }

    /* programs started from the desktop (Terminal) inherit this */
    setenv("BHARAT_DESKTOP", "1", 1);

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
    wm_redraw();
    wm_tick();
    wm_draw();

    long long next_tick = next_wakeup();

    while (!wm_quit && !stop_now) {

        struct pollfd pfd[MAX_FDS];
        int input[MAX_FDS];
        int win_fds[MAX_FDS];
        struct window *owners[MAX_FDS];

        int ni = input_fds(input, MAX_FDS / 2);
        int nw = wm_poll_fds(win_fds, owners, MAX_FDS / 2 - 1);

        for (int i = 0; i < ni; i++)
            pfd[i] = (struct pollfd){.fd = input[i], .events = POLLIN};

        for (int i = 0; i < nw; i++)
            pfd[ni + i] = (struct pollfd){.fd = win_fds[i], .events = POLLIN};

        int watch = ni + nw;

        pfd[watch] = (struct pollfd){.fd = input_watch_fd(), .events = POLLIN};

        /* a System Monitor opened since we last looked needs seconds */
        if (wm_wants_seconds() && next_tick > now_ms() + 1000)
            next_tick = now_ms() + 1000;

        long long wait = next_tick - now_ms();

        if (wait < 0)
            wait = 0;

        /* sleep until something happens; no busy waiting */
        int ready = poll(pfd, watch + 1, (int)wait);

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

        /* a mouse plugged in (or allowed) later is picked up here */
        if (ready > 0 && (pfd[watch].revents & POLLIN))
            input_watch_read();

        wm_cleanup();

        if (now_ms() >= next_tick) {

            wm_tick();
            next_tick = next_wakeup();
        }

        if (wm_needs_redraw())
            wm_draw();
    }

    wm_close_all();
    input_close();
    gfx_close();
    restore_console();

    if (!getenv("BHARAT_SESSION"))
        printf("Desktop closed.\n");

    return wm_exit_code;
}
