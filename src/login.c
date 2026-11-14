/*
 * Bharat-linux login.
 *
 * Asks for a name and password, checks them against /etc/shadow, then
 * BECOMES that user (real uid and gid) before starting the shell. From
 * that moment the kernel itself enforces what the user may touch.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <grp.h>

#include "libuser.h"

int main() {

    char name[64];
    char pass[128];

    while (1) {

        printf("login: ");
        fflush(stdout);

        /* NULL means the terminal was closed: exit so init restarts us */
        if (!fgets(name, sizeof(name), stdin))
            return 1;

        name[strcspn(name, "\r\n")] = 0;

        if (name[0] == 0)
            continue;

        if (!read_hidden("password: ", pass, sizeof(pass)))
            return 1;

        struct bharat_user user;

        int ok = find_user(name, 0, &user) &&
                 check_password(name, pass);

        /* wipe the password from memory as soon as it is checked */
        memset(pass, 0, sizeof(pass));

        if (!ok) {

            /* the pause makes guessing passwords slow */
            sleep(2);

            printf("Login failed\n");

            continue;
        }

        /* the user must own the terminal they are typing on */
        if (fchown(STDIN_FILENO, user.uid, user.gid) != 0) {
            /* not fatal: for example a serial console may refuse */
        }

        /*
         * Drop root. The order matters: groups first, then gid, then uid.
         * After setuid() we can never get root back.
         */
        if (setgroups(0, NULL) != 0 ||
            setgid(user.gid) != 0 ||
            setuid(user.uid) != 0) {

            printf("Cannot switch to user %s\n", name);

            return 1;
        }

        clearenv();

        setenv("USER", user.name, 1);
        setenv("LOGNAME", user.name, 1);
        setenv("HOME", user.home, 1);
        setenv("SHELL", user.shell, 1);
        setenv("PATH", "/bin:/usr/bin", 1);
        setenv("TERM", "linux", 1);

        if (chdir(user.home) != 0 && chdir("/") != 0)
            return 1;

        printf("Welcome %s\n", user.name);
        fflush(stdout);

        execl(user.shell, user.shell, NULL);

        perror("shell failed");

        return 1;
    }
}
