/*
 * sudo <command>
 *
 * Runs one command as root after asking for the root password.
 * The file /bin/sudo is "setuid root": the kernel starts it with root
 * rights no matter who runs it, so it must check the password itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>

#include "libuser.h"

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: sudo <command>\n");

        return 1;
    }

    if (geteuid() != 0) {

        printf("sudo: not installed as setuid root\n");

        return 1;
    }

    /* root does not need to prove anything */
    if (getuid() != 0) {

        char password[128];

        if (!read_hidden("root password: ", password, sizeof(password)))
            return 1;

        int ok = check_password("root", password);

        memset(password, 0, sizeof(password));

        if (!ok) {

            sleep(2);

            printf("sudo authentication failed\n");

            return 1;
        }
    }

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

    execvp(argv[1], &argv[1]);

    printf("sudo: command not found: %s\n", argv[1]);

    return 127;
}
