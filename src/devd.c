/*
 * devd - the device helper (started by init, runs as root).
 *
 * 1. Loads the drivers listed in /etc/modules (one name per line).
 * 2. Gives devices to groups, following /etc/devices:
 *        /dev/fb*            video  660
 *        /dev/input/event*   input  660
 *    so that users in those groups may use the screen and the mouse.
 *    It checks again every 2 seconds, because devices can appear later
 *    (a driver finishing its start-up, a USB mouse plugged in).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "libuser.h"

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

int main(void) {

    if (getuid() != 0) {

        printf("devd must run as root (init starts it)\n");
        return 1;
    }

    load_modules();

    while (1) {

        apply_rules();
        sleep(2);
    }
}
