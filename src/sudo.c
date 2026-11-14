/*
 * sudo <command>      run one command as root
 * sudo -k             forget the password now
 *
 * Who may use it: root, and members of the "wheel" group
 * (administrators). They type THEIR OWN password, not root's, so the
 * root account can stay locked. After a correct password, sudo does not
 * ask again for 5 minutes. Every use and every failure is written to
 * /var/log/auth.log.
 *
 * /bin/sudo is "setuid root": the kernel starts it with root rights no
 * matter who runs it, so it must check everything itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libaccount.h"

#define REMEMBER_SECONDS 300
#define TRIES 3

static void stamp_path(uid_t uid, char *out, size_t size) {

    snprintf(out, size, "/run/sudo/%u", (unsigned)uid);
}

/* Did this user type the right password in the last 5 minutes? */
static int recently_checked(uid_t uid) {

    char path[64];
    struct stat st;

    stamp_path(uid, path, sizeof(path));

    if (stat(path, &st) != 0 || st.st_uid != 0)
        return 0;

    time_t now = time(NULL);

    return st.st_mtime <= now && now - st.st_mtime < REMEMBER_SECONDS;
}

static void remember(uid_t uid) {

    char path[64];

    /* /run is cleared at every boot; only root can look inside */
    mkdir("/run/sudo", 0700);
    stamp_path(uid, path, sizeof(path));

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (fd >= 0)
        close(fd);
}

static void forget(uid_t uid) {

    char path[64];

    stamp_path(uid, path, sizeof(path));
    unlink(path);
}

int main(int argc, char *argv[]) {

    if (geteuid() != 0) {

        printf("sudo: not installed as setuid root\n");
        return 1;
    }

    struct bharat_user me;

    if (!find_user(NULL, getuid(), &me)) {

        printf("sudo: you are not in /etc/passwd\n");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "-k") == 0) {

        forget(getuid());
        return 0;
    }

    if (argc < 2) {

        printf("Usage: sudo <command>    (sudo -k: forget the password)\n");
        return 1;
    }

    char command[256] = "";

    for (int i = 1; i < argc; i++) {

        strncat(command, argv[i], sizeof(command) - strlen(command) - 2);
        strncat(command, " ", sizeof(command) - strlen(command) - 1);
    }

    if (getuid() != 0) {

        if (!is_admin(me.name)) {

            printf("%s is not an administrator, so sudo is not allowed.\n"
                   "This attempt has been logged.\n", me.name);
            auth_log("sudo: DENIED %s (not in %s): %s", me.name, ADMIN_GROUP, command);
            return 1;
        }

        if (!recently_checked(getuid())) {

            int ok = 0;
            char prompt[128], password[128];

            snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", me.name);

            for (int i = 0; i < TRIES && !ok; i++) {

                if (!read_hidden(prompt, password, sizeof(password)))
                    return 1;

                ok = check_password(me.name, password);
                memset(password, 0, sizeof(password));

                if (!ok) {

                    sleep(2);
                    printf("Sorry, try again.\n");
                }
            }

            if (!ok) {

                printf("sudo: %d wrong passwords\n", TRIES);
                auth_log("sudo: %d wrong passwords for %s: %s", TRIES, me.name, command);
                return 1;
            }

            remember(getuid());
        }
    }

    auth_log("sudo: %s ran as root: %s", me.name, command);

    if (setgroups(0, NULL) != 0 ||
        setgid(0) != 0 ||
        setuid(0) != 0) {

        printf("sudo: cannot become root\n");
        return 1;
    }

    /* a clean environment: the caller must not smuggle in PATH tricks */
    clearenv();

    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("HOME", "/home/root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("TERM", "linux", 1);
    setenv("SUDO_USER", me.name, 1);
    load_timezone();

    execvp(argv[1], &argv[1]);

    printf("sudo: command not found: %s\n", argv[1]);

    return 127;
}
