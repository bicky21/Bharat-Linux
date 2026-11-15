/*
 * Bharat-linux init (PID 1)
 *
 * The kernel starts this program first. Its jobs:
 *   1. mount the virtual filesystems (/proc, /sys, /dev, /run, /tmp)
 *   2. set the hostname
 *   3. mount the extra disks listed in /etc/fstab and turn on zram
 *      (compressed memory used as swap)
 *   4. start devd (drivers, device permissions, USB sticks), netd
 *      (the network) and the
 *      service manager
 *   5. keep a login running on the console: the graphical login screen
 *      (greeter) when there is a graphics screen, else the text login
 *   6. collect ("reap") every finished process so no zombies pile up
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/swap.h>
#include <sys/sysinfo.h>

#include "libdisk.h"

void mount_fs(const char *source,
              const char *target,
              const char *type,
              const char *options) {

    mkdir(target, 0755);

    /* EBUSY only means it is already mounted, which is fine */
    if (mount(source, target, type, 0, options) != 0 &&
        errno != EBUSY)
        printf("init: cannot mount %s: %s\n",
               target,
               strerror(errno));
}

void set_hostname() {

    char host[128] = "bharat";

    FILE *f = fopen("/etc/hostname", "r");

    if (f) {

        if (fgets(host, sizeof(host), f))
            host[strcspn(host, "\r\n")] = 0;

        fclose(f);
    }

    if (host[0] && sethostname(host, strlen(host)) != 0)
        printf("init: cannot set hostname\n");
}

/*
 * /dev/console cannot become a "controlling terminal", and without one
 * Ctrl-C does nothing. So find the real device behind the console
 * (for example ttyS0 or tty1) and use that for the login session.
 */
void open_terminal() {

    char active[128] = "";
    char path[160];

    FILE *f = fopen("/sys/class/tty/console/active", "r");

    if (f) {

        if (!fgets(active, sizeof(active), f))
            active[0] = 0;

        fclose(f);
    }

    active[strcspn(active, "\r\n")] = 0;

    /* the file can list several consoles; the last one is the main one */
    char *name = strrchr(active, ' ');

    name = name ? name + 1 : active;

    if (name[0] == 0)
        return;

    if (strcmp(name, "tty0") == 0)
        name = "tty1";

    snprintf(path, sizeof(path), "/dev/%s", name);

    int fd = open(path, O_RDWR);

    if (fd < 0)
        return;

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > STDERR_FILENO)
        close(fd);

    ioctl(STDIN_FILENO, TIOCSCTTY, 1);
}

/* which login runs next on the console */
static int use_text_login;

/* the graphical login needs a screen and the greeter program */
static int graphics_available() {

    return access("/dev/fb0", F_OK) == 0 &&
           access("/bin/greeter", X_OK) == 0;
}

static void banner() {

    printf("\n");
    printf("=================================\n");
    printf("        Bharat Linux            \n");
    printf("=================================\n");
    fflush(stdout);
}

pid_t start_login() {

    int graphical = !use_text_login && graphics_available();

    pid_t pid = fork();

    if (pid == 0) {

        setsid();
        open_terminal();

        if (graphical) {

            /* its error messages go to a log file, not over the picture */
            int log = open("/var/log/greeter.log", O_WRONLY | O_CREAT | O_APPEND, 0600);

            if (log >= 0) {

                dup2(log, STDERR_FILENO);
                close(log);
            }

            execl("/bin/greeter", "/bin/greeter", NULL);
            _exit(3);
        }

        /*
         * If the login screen stopped in graphics mode, the text would be
         * invisible (a black screen): always switch back to text first.
         */
        ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);

        banner();

        /* the live system (USB stick or DVD): say how to log in and install */
        if (access("/etc/live", F_OK) == 0) {

            printf("Live system. Log in as: live  (password: live)\n"
                   "To install Bharat Linux onto a disk: sudo bharat-install\n\n");
            fflush(stdout);
        }

        execl("/bin/login",
              "/bin/login",
              NULL);

        perror("login exec failed");

        _exit(1);
    }

    return pid;
}

/* writes a short text into a /sys file */
static int write_text(const char *path, const char *text) {

    int fd = open(path, O_WRONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);

    close(fd);

    return ok ? 0 : -1;
}

/*
 * zram: a swap area inside memory, compressed. Pages the system has not
 * used for a while are squeezed to about a third of their size, so a
 * small computer behaves as if it had more memory, without wearing out
 * a USB stick or SSD. /etc/zram holds the size as a percentage of the
 * memory (default 50); "0" turns it off.
 */
static void start_zram(void) {

    int percent = 50;
    FILE *f = fopen("/etc/zram", "r");

    if (f) {

        if (fscanf(f, "%d", &percent) != 1)
            percent = 50;

        fclose(f);
    }

    if (percent <= 0 || percent > 150)
        return;

    if (access("/sys/block/zram0", F_OK) != 0) {

        char *argv[] = {"/bin/modprobe", "zram", NULL};

        run_program(argv);
    }

    struct sysinfo si;
    char size[32];

    if (access("/sys/block/zram0/disksize", F_OK) != 0 || sysinfo(&si) != 0)
        return;

    snprintf(size, sizeof(size), "%llu",
             (unsigned long long)si.totalram * si.mem_unit / 100 * (unsigned)percent);

    /* zstd squeezes best; older kernels fall back to their default */
    write_text("/sys/block/zram0/comp_algorithm", "zstd");

    if (write_text("/sys/block/zram0/disksize", size) != 0)
        return;

    char *mk[] = {"/bin/mkswap", "-L", "zram", "/dev/zram0", NULL};
    int quiet = open("/dev/null", O_WRONLY);
    int saved = dup(STDOUT_FILENO);

    /* mkswap prints a line; keep the boot screen clean */
    dup2(quiet, STDOUT_FILENO);
    run_program(mk);
    dup2(saved, STDOUT_FILENO);
    close(quiet);
    close(saved);

    /* priority 100: use the fast memory swap before any disk swap */
    swapon("/dev/zram0", SWAP_FLAG_PREFER | (100 << SWAP_FLAG_PRIO_SHIFT));
}

void start_devd() {

    pid_t pid = fork();

    if (pid == 0) {

        execl("/bin/devd",
              "/bin/devd",
              NULL);

        _exit(1);
    }
}

/* netd: the network (wired cards, DHCP, name servers) */
void start_netd() {

    pid_t pid = fork();

    if (pid == 0) {

        execl("/bin/netd", "/bin/netd", NULL);
        _exit(1);
    }
}

void start_services() {

    pid_t pid = fork();

    if (pid == 0) {

        execl("/bin/serviced",
              "/bin/serviced",
              NULL);

        _exit(1);
    }
}

int main() {

    mount_fs("proc",     "/proc", "proc",     NULL);
    mount_fs("sysfs",    "/sys",  "sysfs",    NULL);
    mount_fs("devtmpfs", "/dev",  "devtmpfs", NULL);

    /* terminals inside the desktop (pseudo terminals) live here */
    mount_fs("devpts", "/dev/pts", "devpts", "mode=620,ptmxmode=666");

    /* /run and /tmp live in memory, so they start empty on every boot */
    mount_fs("tmpfs", "/run", "tmpfs", "mode=0755");
    mount_fs("tmpfs", "/tmp", "tmpfs", "mode=1777");

    /* shared memory between the parts of a program (Tor Browser needs it) */
    mount_fs("tmpfs", "/dev/shm", "tmpfs", "mode=1777,nosuid,nodev");

    setenv("PATH", "/bin:/usr/bin", 1);

    /* the kernel loads some drivers by itself (file systems, crypto) */
    write_text("/proc/sys/kernel/modprobe", "/bin/modprobe");

    set_hostname();

    /* extra disks from /etc/fstab: a missing one never stops the boot */
    mount_fstab(0, 1);
    start_zram();

    /* "ping" for everyone, without root rights (ICMP sockets) */
    write_text("/proc/sys/net/ipv4/ping_group_range", "0 2147483647");

    start_devd();
    start_netd();
    start_services();

    /* "bharat.text" on the kernel command line: the text login only */
    char cmdline[1024] = "";
    FILE *cf = fopen("/proc/cmdline", "r");

    if (cf) {

        if (!fgets(cmdline, sizeof(cmdline), cf))
            cmdline[0] = 0;

        fclose(cf);
    }

    if (strstr(cmdline, "bharat.text"))
        use_text_login = 1;

    int graphical = !use_text_login && graphics_available();
    pid_t login = start_login();
    time_t started = time(NULL);
    int quick_failures = 0;

    while (1) {

        /* wait() returns for ANY finished child, including orphans */
        int status = 0;
        pid_t done = wait(&status);

        if (done < 0) {

            sleep(1);

            if (errno != ECHILD)
                continue;

            done = login;
        }

        if (done == login) {

            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            if (graphical) {

                /*
                 * 2 = the user pressed "Text mode" on the login screen.
                 * 3 = no screen after all. A greeter that keeps dying
                 * within seconds also means: use the text login.
                 */
                if (code == 2 || code == 3)
                    use_text_login = 1;
                else if (time(NULL) - started < 5 && ++quick_failures >= 3)
                    use_text_login = 1;

                graphical = 0;
            }
            else {

                /* after logging out of the text console, the login screen returns */
                use_text_login = 0;
                quick_failures = 0;
            }

            /* small pause so a broken login cannot spin the CPU */
            sleep(1);

            graphical = !use_text_login && graphics_available();
            started = time(NULL);
            login = start_login();
        }
    }

    return 0;
}
