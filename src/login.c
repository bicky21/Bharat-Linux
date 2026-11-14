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
         * The extra groups (from /etc/group) decide for example who may
         * use the screen (video) and the mouse (input).
         * After setuid() we can never get root back.
         */
        gid_t groups[32];
        int group_count = user_groups(user.name, groups, 32);

        if (setgroups(group_count, groups) != 0 ||
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

        /* the time zone, written like "IST-5:30" (5:30 ahead of UTC) */
        FILE *tz = fopen("/etc/timezone", "r");
        char zone[64];

        if (tz) {

            if (fgets(zone, sizeof(zone), tz)) {

                zone[strcspn(zone, "\r\n")] = 0;
                setenv("TZ", zone, 1);
            }

            fclose(tz);
        }

        if (chdir(user.home) != 0 && chdir("/") != 0)
            return 1;

        printf("Welcome %s\n", user.name);

        /* only here, on the text login: never inside the desktop's Terminal */
        if (access("/bin/desktop", X_OK) == 0)
            printf("Type desktop to start the graphical desktop.\n");

        fflush(stdout);

        execl(user.shell, user.shell, NULL);

        perror("shell failed");

        return 1;
    }
}
