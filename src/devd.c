/*
 * devd - the device helper (started by init, runs as root).
 *
 * 1. Loads the drivers listed in /etc/modules (one name per line).
 * 2. Gives devices to groups, following /etc/devices:
 *        /dev/fb*            video  660
 *        /dev/input/event*   input  660
 *    so that users in those groups may use the screen and the mouse.
 * 3. Opens USB sticks and memory cards for the person using the
 *    desktop: a stick called PENDRIVE appears in /media/ravi/PENDRIVE.
 *    It is mounted so that nothing on it can run as a program or
 *    gain extra rights (noexec, nosuid, nodev).
 * 4. Safely removes them: "eject sdb" (or the Eject button) asks devd
 *    through /run/devd.sock. devd checks who is asking: the person at
 *    the desktop may eject USB sticks, only root may eject other disks.
 *
 * Devices can appear later (a driver finishing its start-up, a USB stick
 * plugged in). The kernel tells us about new files in /dev through
 * inotify, so devd sleeps until then and uses no processor.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "libuser.h"
#include "libdisk.h"

#define SOCKET_PATH "/run/devd.sock"
#define SESSION_FILE "/run/session-user"

static void load_modules(void) {

    FILE *f = fopen("/etc/modules", "r");

    if (!f)
        return;

    char name[128];

    while (fgets(name, sizeof(name), f)) {

        name[strcspn(name, " \t\r\n#")] = 0;

        if (name[0] == 0)
            continue;

        pid_t pid = fork();

        if (pid == 0) {

            execl("/bin/modprobe", "modprobe", name, NULL);
            _exit(127);
        }

        if (pid > 0)
            waitpid(pid, NULL, 0);
    }

    fclose(f);
}

static void apply_rules(void) {

    FILE *f = fopen("/etc/devices", "r");

    if (!f)
        return;

    char line[256], pattern[200], group[64];
    unsigned int mode;

    while (fgets(line, sizeof(line), f)) {

        if (line[0] == '#' ||
            sscanf(line, "%199s %63s %o", pattern, group, &mode) != 3)
            continue;

        gid_t gid;

        if (!find_group(group, &gid))
            continue;

        glob_t g;

        if (glob(pattern, 0, NULL, &g) != 0)
            continue;

        for (size_t i = 0; i < g.gl_pathc; i++) {

            struct stat st;

            if (stat(g.gl_pathv[i], &st) != 0)
                continue;

            /* only touch it when something is different */
            if (st.st_gid != gid || (st.st_mode & 07777) != mode) {

                if (chown(g.gl_pathv[i], 0, gid) == 0)
                    chmod(g.gl_pathv[i], mode);
            }
        }

        globfree(&g);
    }

    fclose(f);
}

/* ====================================================================
 * USB sticks
 * ==================================================================== */

struct stick {
    int used;
    char dev[64];          /* /dev/sdb1 */
    char disk[32];         /* sdb */
    char dir[300];         /* /media/ravi/PENDRIVE */
};

static struct stick sticks[32];
static char session[64];   /* who is at the desktop ("" = nobody) */
static struct bharat_user session_user;

/* is this disk plugged in by USB, or a removable card? */
static int is_removable_disk(const char *disk) {

    char path[128], link[PATH_MAX];

    if (!strncmp(disk, "sr", 2))
        return 0;      /* CDs/DVDs: not opened by themselves */

    snprintf(path, sizeof(path), "/sys/class/block/%s", disk);

    ssize_t n = readlink(path, link, sizeof(link) - 1);

    if (n > 0) {

        link[n] = 0;

        if (strstr(link, "/usb"))
            return 1;
    }

    snprintf(path, sizeof(path), "/sys/block/%s/removable", disk);

    return disk_read_number(path) == 1;
}

/* "MY STICK/2" -> "MY_STICK_2": a name that is easy to type */
static void safe_name(const char *in, char *out, size_t size) {

    size_t k = 0;

    for (size_t i = 0; in[i] && k + 1 < size; i++) {

        char c = in[i];

        out[k++] = (c == '/' || c == ' ' || c < 32 || c == '\'' || c == '"') ? '_' : c;
    }

    out[k] = 0;

    if (!strcmp(out, ".") || !strcmp(out, ".."))
        out[0] = 0;
}

static void open_stick(const struct blockdev *b, const char *disk) {

    char dev[64], mp[256];
    struct fs_info fs;

    snprintf(dev, sizeof(dev), "/dev/%.31s", b->name);

    if (probe_fs(dev, &fs) != 0 || !strcmp(fs.type, "swap"))
        return;

    mount_point_of(dev, mp, sizeof(mp));

    if (mp[0])
        return;     /* already open (by fstab, or by hand) */

    /* /media/ravi: only ravi may look inside */
    char base[160], name[80], dir[300];

    mkdir("/media", 0755);
    snprintf(base, sizeof(base), "/media/%s", session);
    mkdir(base, 0750);

    if (chown(base, 0, session_user.gid) != 0 || chmod(base, 0750) != 0)
        return;

    safe_name(fs.label, name, sizeof(name));

    if (!name[0])
        snprintf(name, sizeof(name), "%s", !strcmp(fs.type, "iso9660") ? "CD" : "USB-DISK");

    snprintf(dir, sizeof(dir), "%s/%s", base, name);

    for (int k = 2; k < 20 && mkdir(dir, 0700) != 0; k++)
        snprintf(dir, sizeof(dir), "%s/%s-%d", base, name, k);

    unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME;
    char data[160] = "";
    int owned = 0;

    if (!strcmp(fs.type, "vfat") || !strcmp(fs.type, "exfat") || !strcmp(fs.type, "ntfs3")) {

        /* these have no owners of their own: everything belongs to the user */
        snprintf(data, sizeof(data), "uid=%u,gid=%u,fmask=0077,dmask=0077",
                 (unsigned)session_user.uid, (unsigned)session_user.gid);
        owned = 1;
    }
    else if (!strcmp(fs.type, "iso9660")) {

        flags |= MS_RDONLY;
        snprintf(data, sizeof(data), "uid=%u,gid=%u",
                 (unsigned)session_user.uid, (unsigned)session_user.gid);
        owned = 1;
    }

    int ok = 0;

    /* utf8: names with Hindi or Bengali letters show up right on FAT */
    if (!strcmp(fs.type, "vfat")) {

        char more[200];

        snprintf(more, sizeof(more), "%s,utf8", data);
        ok = mount(dev, dir, fs.type, flags, more) == 0;

        if (!ok) {

            snprintf(more, sizeof(more), "%s,iocharset=utf8", data);
            ok = mount(dev, dir, fs.type, flags, more) == 0;
        }
    }

    if (!ok)
        ok = mount(dev, dir, fs.type, flags, data[0] ? data : NULL) == 0;

    /* a stick with errors: open it read-only rather than not at all */
    if (!ok && !(flags & MS_RDONLY))
        ok = mount(dev, dir, fs.type, flags | MS_RDONLY, data[0] ? data : NULL) == 0;

    if (!ok) {

        rmdir(dir);
        return;
    }

    if (!owned) {

        /* ext4 sticks keep their owners; a new, empty one gets the user */
        struct stat st;

        if (stat(dir, &st) == 0 && st.st_uid == 0 && st.st_nlink <= 3) {

            DIR *d = opendir(dir);
            struct dirent *e;
            int files = 0;

            while (d && (e = readdir(d)) != NULL)
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "lost+found"))
                    files++;

            if (d)
                closedir(d);

            if (files == 0 && chown(dir, session_user.uid, session_user.gid) != 0) {
                /* not fatal */
            }
        }
    }

    for (int i = 0; i < 32; i++) {

        if (!sticks[i].used) {

            sticks[i].used = 1;
            snprintf(sticks[i].dev, sizeof(sticks[i].dev), "%s", dev);
            snprintf(sticks[i].disk, sizeof(sticks[i].disk), "%.31s", disk);
            snprintf(sticks[i].dir, sizeof(sticks[i].dir), "%s", dir);
            break;
        }
    }
}

static void close_stick(struct stick *s, int lazy) {

    sync();

    if (umount2(s->dir, lazy ? MNT_DETACH : 0) != 0 && !lazy)
        return;

    rmdir(s->dir);
    s->used = 0;
}

/* opens every USB stick that is not open yet */
static void scan_sticks(void) {

    if (!session[0])
        return;

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    for (int i = 0; i < n; i++) {

        const char *disk = list[i].partno ? list[i].parent : list[i].name;

        if (!is_removable_disk(disk))
            continue;

        /* a whole disk with parts: open the parts instead */
        if (!list[i].partno && i + 1 < n && !strcmp(list[i + 1].parent, list[i].name))
            continue;

        open_stick(&list[i], disk);
    }
}

/* sticks pulled out without "eject": let go of them */
static void forget_removed(void) {

    for (int i = 0; i < 32; i++) {

        if (sticks[i].used && access(sticks[i].dev, F_OK) != 0)
            close_stick(&sticks[i], 1);
    }
}

/* the person at the desktop logged in or out */
static void session_changed(void) {

    char who[64] = "";
    FILE *f = fopen(SESSION_FILE, "r");

    if (f) {

        if (fgets(who, sizeof(who), f))
            who[strcspn(who, "\n")] = 0;

        fclose(f);
    }

    if (!strcmp(who, session))
        return;

    /* the last person's sticks are closed: the next one must not see them */
    for (int i = 0; i < 32; i++)
        if (sticks[i].used)
            close_stick(&sticks[i], 1);

    session[0] = 0;

    if (who[0] && find_user(who, (uid_t)-1, &session_user) && session_user.uid != 0)
        snprintf(session, sizeof(session), "%s", who);

    scan_sticks();
}

/* ====================================================================
 * eject requests
 * ==================================================================== */

/* "sdb", "/dev/sdb1", "/media/ravi/PENDRIVE" -> "sdb" */
static int which_disk(const char *what, char *disk, size_t size) {

    char dev[300] = "";

    if (!strncmp(what, "/dev/", 5))
        snprintf(dev, sizeof(dev), "%s", what + 5);
    else if (what[0] == '/') {

        char real[PATH_MAX];

        if (!realpath(what, real))
            snprintf(real, sizeof(real), "%s", what);

        FILE *f = fopen("/proc/mounts", "r");
        char d[256], dir[256];

        while (f && fscanf(f, "%255s %255s %*s %*s %*d %*d", d, dir) == 2) {

            if (!strcmp(dir, real) && !strncmp(d, "/dev/", 5))
                snprintf(dev, sizeof(dev), "%s", d + 5);
        }

        if (f)
            fclose(f);
    }
    else
        snprintf(dev, sizeof(dev), "%s", what);

    if (!dev[0])
        return -1;

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    for (int i = 0; i < n; i++) {

        if (!strcmp(list[i].name, dev)) {

            snprintf(disk, size, "%s", list[i].partno ? list[i].parent : list[i].name);
            return 0;
        }
    }

    return -1;
}

static void eject(const char *what, uid_t asker, char *reply, size_t size) {

    char disk[32];

    if (which_disk(what, disk, sizeof(disk)) != 0) {

        snprintf(reply, size, "error %s: no such disk\n", what);
        return;
    }

    int removable = is_removable_disk(disk);

    if (asker != 0 && !(removable && session[0] && asker == session_user.uid)) {

        snprintf(reply, size, "error only root may remove %s (try: sudo eject %s)\n", disk, disk);
        return;
    }

    /* close every part of the disk that is open */
    sync();

    FILE *f = fopen("/proc/mounts", "r");
    char d[256], dir[256], busy[256] = "";
    char dirs[16][256];
    int count = 0;

    while (f && count < 16 && fscanf(f, "%255s %255s %*s %*s %*d %*d", d, dir) == 2) {

        if (!strncmp(d, "/dev/", 5) && !strncmp(d + 5, disk, strlen(disk)))
            snprintf(dirs[count++], 256, "%s", dir);
    }

    if (f)
        fclose(f);

    for (int i = count - 1; i >= 0; i--) {

        if (!strcmp(dirs[i], "/")) {

            snprintf(reply, size, "error %s holds the running system\n", disk);
            return;
        }

        if (umount(dirs[i]) != 0)
            snprintf(busy, sizeof(busy), "%s", dirs[i]);
    }

    if (busy[0]) {

        snprintf(reply, size, "error %s is still in use: close the files and folders open on it\n", busy);
        return;
    }

    for (int i = 0; i < 32; i++) {

        if (sticks[i].used && !strcmp(sticks[i].disk, disk)) {

            rmdir(sticks[i].dir);
            sticks[i].used = 0;
        }
    }

    /* switch the USB stick off, so it is safe to pull out */
    if (removable) {

        char path[128];
        int fd;

        snprintf(path, sizeof(path), "/sys/block/%s/device/delete", disk);
        fd = open(path, O_WRONLY | O_CLOEXEC);

        if (fd >= 0) {

            if (write(fd, "1", 1) != 1) {
                /* still unmounted, which is what matters */
            }

            close(fd);
        }
    }

    snprintf(reply, size, "ok It is now safe to remove %s.\n", disk);
}

static int open_socket(void) {

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};

    snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
    unlink(SOCKET_PATH);

    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {

        if (s >= 0)
            close(s);

        return -1;
    }

    /* anyone may ask; devd checks who is asking before doing anything */
    chmod(SOCKET_PATH, 0666);

    return s;
}

static void answer(int server) {

    int c = accept4(server, NULL, NULL, SOCK_CLOEXEC);

    if (c < 0)
        return;

    struct ucred who;
    socklen_t len = sizeof(who);
    char req[400] = "", reply[400];

    /* the kernel tells us, truthfully, which user is on the other end */
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &who, &len) != 0) {

        close(c);
        return;
    }

    struct pollfd p = {.fd = c, .events = POLLIN};
    ssize_t n = poll(&p, 1, 2000) > 0 ? read(c, req, sizeof(req) - 1) : -1;

    if (n > 0) {

        req[n] = 0;
        req[strcspn(req, "\n")] = 0;

        if (!strncmp(req, "eject ", 6))
            eject(req + 6, who.uid, reply, sizeof(reply));
        else
            snprintf(reply, sizeof(reply), "error unknown request\n");

        if (write(c, reply, strlen(reply)) < 0) {
            /* the asker went away */
        }
    }

    close(c);
}

/* ====================================================================
 * main loop
 * ==================================================================== */

/* the folders where device files appear */
static const char *watched[] = {"/dev", "/dev/input", "/dev/dri"};

static void watch_folders(int fd) {

    for (int i = 0; i < 3; i++)
        inotify_add_watch(fd, watched[i], IN_CREATE | IN_DELETE);   /* fails quietly if missing */

    inotify_add_watch(fd, "/run", IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
}

int main(void) {

    if (getuid() != 0) {

        printf("devd must run as root (init starts it)\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int fd = inotify_init1(IN_CLOEXEC);

    if (fd >= 0)
        watch_folders(fd);

    int server = open_socket();

    /* first, so "lsblk -f" works at once; drivers can take a while */
    write_disk_cache();
    load_modules();
    session_changed();

    while (1) {

        apply_rules();
        forget_removed();
        write_disk_cache();
        scan_sticks();

        /* sleep until something happens (at most 5 minutes) */
        struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = server, .events = POLLIN}};

        if (poll(p, 2, 5 * 60 * 1000) <= 0)
            continue;

        if (p[1].revents & POLLIN)
            answer(server);

        if (p[0].revents & POLLIN) {

            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t n = read(fd, buf, sizeof(buf));
            int who_changed = 0;

            for (char *e = buf; n > 0 && e < buf + n;) {

                struct inotify_event *ev = (struct inotify_event *)e;

                if (ev->len && !strcmp(ev->name, "session-user"))
                    who_changed = 1;

                e += sizeof(*ev) + ev->len;
            }

            /* a folder such as /dev/input may have just been created */
            watch_folders(fd);

            if (who_changed)
                session_changed();

            /* give the kernel a moment to finish creating the device */
            usleep(200 * 1000);
        }
    }
}
