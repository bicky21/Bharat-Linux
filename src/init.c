/*
 * Bharat-linux init (PID 1)
 *
 * The kernel starts this program first. Its jobs:
 *   1. mount the virtual filesystems (/proc, /sys, /dev, /run, /tmp)
 *   2. set the hostname
 *   3. start the service manager
 *   4. keep a login prompt running on the console
 *   5. collect ("reap") every finished process so no zombies pile up
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

pid_t start_login() {

    pid_t pid = fork();

    if (pid == 0) {

        setsid();
        open_terminal();

        execl("/bin/login",
              "/bin/login",
              NULL);

        perror("login exec failed");

        _exit(1);
    }

    return pid;
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

    /* /run and /tmp live in memory, so they start empty on every boot */
    mount_fs("tmpfs", "/run", "tmpfs", "mode=0755");
    mount_fs("tmpfs", "/tmp", "tmpfs", "mode=1777");

    /* the process list used by ps and kill */
    close(open("/run/processes.db", O_WRONLY | O_CREAT, 0644));

    setenv("PATH", "/bin:/usr/bin", 1);

    set_hostname();

    printf("\n");
    printf("=================================\n");
    printf("        Bharat-linux CLI        \n");
    printf("=================================\n");
    fflush(stdout);

    start_services();

    pid_t login = start_login();

    while (1) {

        /* wait() returns for ANY finished child, including orphans */
        pid_t done = wait(NULL);

        if (done < 0) {

            sleep(1);

            if (errno != ECHILD)
                continue;

            done = login;
        }

        if (done == login) {

            /* small pause so a broken login cannot spin the CPU */
            sleep(1);

            login = start_login();
        }
    }

    return 0;
}
