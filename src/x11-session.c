/*
 * x11-session - runs Tor Browser (or another X11 program) full screen.
 *
 *   x11-session USER                  Tor Browser for USER
 *   x11-session USER -- PROGRAM ...   another program (for testing)
 *
 * Started by the login screen (as root) when someone presses "Tor
 * Browser" on the desktop. Tor Browser is the official build from the
 * Tor Project: it needs the X11 display system, which Bharat Linux
 * does not use otherwise. So, for as long as the browser is open:
 *
 *   1. an X server (Xorg) takes over the screen, keyboard and mouse,
 *      with a fixed configuration (no udev): the frame buffer /dev/fb0
 *      and every /dev/input/event* device
 *   2. it only accepts programs that know a secret (an X "cookie"),
 *      and never listens on the network
 *   3. Tor Browser runs as the user, from the user's own folder in
 *      /var/lib/tor-browser/USER (see prepare_copy)
 *   4. when it closes, the X server stops and the desktop comes back
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <dirent.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "libuser.h"

#define XORG       "/usr/lib/xorg/Xorg"
#define XAUTH      "/usr/bin/xauth"
#define XWM        "/usr/lib/bharat/bharat-xwm"
#define TB_SYSTEM  "/opt/tor-browser"
#define RUN        "/run/x11"

static void say(const char *msg) {

    fprintf(stderr, "x11-session: %s\n", msg);
}

/* ---------- the X server ---------- */

static int write_config(void) {

    FILE *f = fopen(RUN "/xorg.conf", "w");
    glob_t g;
    int n = 0;

    if (!f)
        return -1;

    fprintf(f,
            "# Written by x11-session: no automatic devices (there is no udev)\n"
            "Section \"ServerFlags\"\n"
            "    Option \"AutoAddDevices\" \"false\"\n"
            "    Option \"AutoEnableDevices\" \"false\"\n"
            "    Option \"DontVTSwitch\" \"true\"\n"
            "    Option \"DontZap\" \"true\"\n"
            "    Option \"BlankTime\" \"0\"\n"
            "EndSection\n\n"
            "Section \"Device\"\n    Identifier \"screen0\"\n    Driver \"fbdev\"\n"
            "    Option \"fbdev\" \"/dev/fb0\"\nEndSection\n\n"
            "Section \"Screen\"\n    Identifier \"main\"\n    Device \"screen0\"\nEndSection\n\n");

    if (glob("/dev/input/event*", 0, NULL, &g) == 0) {

        for (size_t i = 0; i < g.gl_pathc; i++, n++)
            fprintf(f, "Section \"InputDevice\"\n    Identifier \"input%d\"\n    Driver \"evdev\"\n"
                       "    Option \"Device\" \"%s\"\nEndSection\n\n", n, g.gl_pathv[i]);

        globfree(&g);
    }

    fprintf(f, "Section \"ServerLayout\"\n    Identifier \"layout\"\n    Screen \"main\"\n");

    for (int i = 0; i < n; i++)
        fprintf(f, "    InputDevice \"input%d\" \"SendCoreEvents\"\n", i);

    fprintf(f, "EndSection\n");

    return fclose(f);
}

static int run_wait(char *const argv[]) {

    pid_t pid = fork();

    if (pid == 0) {

        int null = open("/dev/null", O_WRONLY);

        dup2(null, STDOUT_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }

    int st;

    return pid > 0 && waitpid(pid, &st, 0) > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* a random secret, as 32 hex digits */
static int make_cookie(char *out) {

    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0 || read(fd, r, 16) != 16) {

        if (fd >= 0)
            close(fd);

        return -1;
    }

    close(fd);

    for (int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02x", r[i]);

    return 0;
}

static pid_t start_x(void) {

    unlink("/tmp/.X11-unix/X0");
    unlink("/tmp/.X0-lock");
    mkdir("/tmp/.X11-unix", 01777);
    chmod("/tmp/.X11-unix", 01777);

    pid_t pid = fork();

    if (pid == 0) {

        /* Xorg's messages go to its log, not over the screen */
        int null = open("/dev/null", O_RDWR);

        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);

        execl(XORG, XORG, ":0", "-config", RUN "/xorg.conf", "-nolisten", "tcp",
              "-auth", RUN "/serverauth", "-logfile", RUN "/Xorg.log",
              "-noreset", "-keeptty", "-novtswitch", "vt1", NULL);
        _exit(127);
    }

    /* ready when its socket appears (at most 20 seconds) */
    for (int i = 0; i < 200; i++) {

        int st;

        if (access("/tmp/.X11-unix/X0", F_OK) == 0)
            return pid;

        if (waitpid(pid, &st, WNOHANG) == pid) {

            say("the X server stopped at once (see /run/x11/Xorg.log)");
            return -1;
        }

        usleep(100 * 1000);
    }

    kill(pid, SIGTERM);
    say("the X server did not start in time");

    return -1;
}

static void stop_x(pid_t pid) {

    kill(pid, SIGTERM);

    for (int i = 0; i < 50; i++) {

        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;

        usleep(100 * 1000);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ---------- the user's copy of Tor Browser ---------- */

/*
 * Tor Browser is a "portable" program: it keeps its settings and Tor's
 * state inside its own folder. Each user gets their own folder in
 * /var/lib/tor-browser/USER, made by root (this program) so the user
 * cannot change the program itself:
 *
 *   app/                the program: hard links to /opt/tor-browser
 *                       (no extra disk or memory), owned by root
 *   app/Browser/TorBrowser/Data   the user's settings and Tor's state,
 *                       a real copy owned by the user, kept on updates
 *   home/               the browser's HOME (caches), owned by the user
 */

#define TB_USERS "/var/lib/tor-browser"
#define DATA     "Browser/TorBrowser/Data"

static const struct bharat_user *owner;     /* for real copies */

static int link_or_copy(const char *from, const char *to, const struct stat *st, int real_copy) {

    if (!real_copy && link(from, to) == 0)
        return 0;

    int in = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, st->st_mode & 0777);
    char buf[1 << 16];
    ssize_t n = 0;

    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) {

            n = -1;
            break;
        }

    if (real_copy && out >= 0 && fchown(out, owner->uid, owner->gid) != 0)
        n = -1;

    if (in >= 0) close(in);
    if (out >= 0) close(out);

    return in < 0 || out < 0 || n < 0 ? -1 : 0;
}

/* rel: the path inside the bundle, to find the Data folder */
static int copy_tree(const char *from, const char *to, const char *rel, int real_copy) {

    struct stat st;

    if (lstat(from, &st) != 0)
        return -1;

    if (!real_copy && !strcmp(rel, DATA))
        return 0;                       /* the user's data is handled apart */

    if (S_ISLNK(st.st_mode)) {

        char target[4096];
        ssize_t n = readlink(from, target, sizeof(target) - 1);

        if (n < 0)
            return -1;

        target[n] = 0;

        if (symlink(target, to) != 0)
            return -1;

        if (real_copy && lchown(to, owner->uid, owner->gid) != 0)
            return -1;

        return 0;
    }

    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode) ? link_or_copy(from, to, &st, real_copy) : 0;

    if (mkdir(to, real_copy ? 0700 : 0755) != 0)
        return -1;

    if (real_copy && chown(to, owner->uid, owner->gid) != 0)
        return -1;

    DIR *d = opendir(from);
    struct dirent *e;
    int bad = d ? 0 : -1;

    while (d && (e = readdir(d)) != NULL) {

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char a[4096], b[4096], r[4096];

        snprintf(a, sizeof(a), "%.2000s/%.1000s", from, e->d_name);
        snprintf(b, sizeof(b), "%.2000s/%.1000s", to, e->d_name);
        snprintf(r, sizeof(r), "%.2000s%s%.1000s", rel, rel[0] ? "/" : "", e->d_name);
        bad |= copy_tree(a, b, r, real_copy);
    }

    if (d)
        closedir(d);

    return bad;
}

static int remove_one(const char *path, const struct stat *st, int type, struct FTW *f) {

    (void)st; (void)f;

    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void remove_tree(const char *path) {

    nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void read_line(const char *path, char *out, size_t size) {

    FILE *f = fopen(path, "r");

    out[0] = 0;

    if (f) {

        if (!fgets(out, (int)size, f))
            out[0] = 0;

        fclose(f);
    }
}

/* runs as root: makes or updates /var/lib/tor-browser/USER */
static int prepare_copy(const struct bharat_user *user, char *base, size_t size) {

    char path[4096], app[4096], fresh[4096], old[4096], stamp_sys[128], stamp_user[128];

    owner = user;
    mkdir(TB_USERS, 0755);
    snprintf(base, size, TB_USERS "/%s", user->name);
    mkdir(base, 0755);

    struct stat st;

    if (lstat(base, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0)
        return -1;

    /* the browser's HOME */
    snprintf(path, sizeof(path), "%s/home", base);
    mkdir(path, 0700);

    if (lchown(path, user->uid, user->gid) != 0)
        return -1;

    read_line(TB_SYSTEM "/.version", stamp_sys, sizeof(stamp_sys));
    snprintf(path, sizeof(path), "%s/.version", base);
    read_line(path, stamp_user, sizeof(stamp_user));
    snprintf(app, sizeof(app), "%s/app", base);

    if (stamp_user[0] && !strcmp(stamp_sys, stamp_user) && access(app, F_OK) == 0)
        return 0;

    /* new, or a newer Tor Browser: build the program folder afresh */
    snprintf(fresh, sizeof(fresh), "%s/app.new", base);
    snprintf(old, sizeof(old), "%s/app.old", base);
    remove_tree(fresh);
    remove_tree(old);

    if (copy_tree(TB_SYSTEM, fresh, "", 0) != 0)
        return -1;

    /* the user's data: moved over from the old folder, or a first copy */
    char from[4200], to[4200];

    snprintf(from, sizeof(from), "%s/" DATA, app);
    snprintf(to, sizeof(to), "%s/" DATA, fresh);

    if (rename(from, to) != 0) {

        snprintf(from, sizeof(from), TB_SYSTEM "/" DATA);

        if (copy_tree(from, to, DATA, 1) != 0)
            return -1;
    }

    rename(app, old);

    if (rename(fresh, app) != 0)
        return -1;

    remove_tree(old);

    FILE *f = fopen(path, "w");

    if (f) {

        fputs(stamp_sys, f);
        fclose(f);
    }

    return 0;
}

/* the browser program in the bundle (an ELF file, not a shell script) */
static int is_elf(const char *path) {

    char magic[4];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int ok = fd >= 0 && read(fd, magic, 4) == 4 && !memcmp(magic, "\177ELF", 4);

    if (fd >= 0)
        close(fd);

    return ok;
}

/* runs as the user */
static void run_tor_browser(const char *base) {

    char browser[4200], lib[9000], fc[4300], profile[4300], bin[4300], home[4200];

    snprintf(browser, sizeof(browser), "%s/app/Browser", base);
    snprintf(home, sizeof(home), "%s/home", base);

    if (chdir(browser) != 0) {

        say("Tor Browser is not complete");
        _exit(1);
    }

    /* what Tor Browser's own start-tor-browser script sets up */
    snprintf(lib, sizeof(lib), "%s:%s/TorBrowser/Tor", browser, browser);
    snprintf(fc, sizeof(fc), "%s/fontconfig", browser);
    snprintf(profile, sizeof(profile), "%s/TorBrowser/Data/Browser/profile.default", browser);

    setenv("HOME", home, 1);            /* nothing about the user's real home leaks */
    setenv("LD_LIBRARY_PATH", lib, 1);
    setenv("FONTCONFIG_PATH", fc, 1);
    setenv("FONTCONFIG_FILE", "fonts.conf", 1);
    setenv("GSETTINGS_BACKEND", "memory", 1);
    setenv("MOZ_ENABLE_WAYLAND", "0", 1);
    setenv("GDK_BACKEND", "x11", 1);
    unsetenv("SESSION_MANAGER");

    snprintf(bin, sizeof(bin), "%s/firefox.real", browser);

    if (!is_elf(bin))
        snprintf(bin, sizeof(bin), "%s/firefox", browser);

    execl(bin, bin, "--class", "Tor Browser", "-profile", profile, NULL);
    say("Tor Browser did not start");
    _exit(127);
}

int main(int argc, char **argv) {

    if (argc < 2 || getuid() != 0) {

        fprintf(stderr, "usage (as root): x11-session USER [-- PROGRAM ARGS]\n");
        return 2;
    }

    struct bharat_user user;

    if (!find_user(argv[1], (uid_t)-1, &user)) {

        say("no such user");
        return 1;
    }

    if (access(XORG, X_OK) != 0) {

        say("the Privacy Pack (X11) is not installed: build it with make torbrowser");
        return 1;
    }

    signal(SIGINT, SIG_IGN);

    /* what happens is written to /run/x11/session.log (for "it did not start") */
    mkdir(RUN, 0755);
    chmod(RUN, 0755);
    umask(022);

    int log = open(RUN "/session.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (log >= 0) {

        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
        close(log);
    }

    char cookie[40];

    if (write_config() != 0 || make_cookie(cookie) != 0) {

        say("could not prepare the X server");
        return 1;
    }

    /* the server's copy of the secret (root only) */
    unlink(RUN "/serverauth");

    char *server_auth[] = {XAUTH, "-q", "-f", RUN "/serverauth", "add", ":0", "MIT-MAGIC-COOKIE-1", cookie, NULL};

    if (run_wait(server_auth) != 0) {

        say("xauth failed");
        return 1;
    }

    char base[3000] = "";
    int testing = argc > 3 && !strcmp(argv[2], "--");

    if (!testing && (access(TB_SYSTEM "/Browser", F_OK) != 0 || prepare_copy(&user, base, sizeof(base)) != 0)) {

        say("Tor Browser is not installed, or the disk is full");
        return 1;
    }

    pid_t x = start_x();

    if (x < 0)
        return 1;

    /* our tiny window manager: Tor Browser fills the screen */
    pid_t wm = fork();

    if (wm == 0) {

        setenv("DISPLAY", ":0", 1);
        setenv("XAUTHORITY", RUN "/serverauth", 1);
        execl(XWM, "bharat-xwm", NULL);
        _exit(127);
    }

    pid_t app = fork();

    if (app == 0) {

        if (become_user(&user) != 0)
            _exit(1);

        char xa[3200];

        /* the user's copy of the secret */
        snprintf(xa, sizeof(xa), "%s/.Xauthority", testing ? user.home : base);
        if (!testing)
            snprintf(xa, sizeof(xa), "%s/home/.Xauthority", base);
        unlink(xa);

        char *user_auth[] = {XAUTH, "-q", "-f", xa, "add", ":0", "MIT-MAGIC-COOKIE-1", cookie, NULL};

        run_wait(user_auth);
        setenv("DISPLAY", ":0", 1);
        setenv("XAUTHORITY", xa, 1);
        setenv("NO_AT_BRIDGE", "1", 1);     /* no accessibility bus here: do not wait for it */

        if (testing) {

            execvp(argv[3], argv + 3);
            _exit(127);
        }

        run_tor_browser(base);
    }

    int st = 0;

    if (app > 0)
        waitpid(app, &st, 0);

    if (wm > 0) {

        kill(wm, SIGTERM);
        waitpid(wm, NULL, 0);
    }

    stop_x(x);
    unlink(RUN "/serverauth");

    return app > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
