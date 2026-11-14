/*
 * ps      show the processes that have a command line (programs)
 * ps -a   also show kernel threads
 *
 * Everything is read live from /proc, the kernel's own process list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "libuser.h"

int main(int argc, char *argv[]) {

    int all = argc > 1 && strcmp(argv[1], "-a") == 0;

    DIR *d = opendir("/proc");

    if (!d) {

        printf("Cannot open /proc\n");

        return 1;
    }

    printf("%6s %-10s %s\n", "PID", "USER", "COMMAND");

    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {

        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;

        char path[300];
        struct stat st;

        snprintf(path, sizeof(path), "/proc/%s", entry->d_name);

        /* the owner of /proc/<pid> is the user running the process */
        if (stat(path, &st) != 0)
            continue;

        struct bharat_user user;
        char name[64];

        if (find_user(NULL, st.st_uid, &user))
            snprintf(name, sizeof(name), "%s", user.name);
        else
            snprintf(name, sizeof(name), "%d", (int)st.st_uid);

        char cmd[256] = "";

        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);

        FILE *f = fopen(path, "r");

        if (f) {

            size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);

            fclose(f);

            /* arguments are separated by 0 bytes: turn them into spaces */
            for (size_t i = 0; i + 1 < n; i++) {

                if (cmd[i] == 0)
                    cmd[i] = ' ';
            }

            cmd[n] = 0;
        }

        if (cmd[0] == 0) {

            if (!all)
                continue;

            /* kernel threads have no command line; show their name */
            snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);

            f = fopen(path, "r");

            if (f) {

                char comm[64] = "";

                if (fgets(comm, sizeof(comm), f))
                    comm[strcspn(comm, "\n")] = 0;

                fclose(f);

                snprintf(cmd, sizeof(cmd), "[%s]", comm);
            }
        }

        printf("%6s %-10s %s\n", entry->d_name, name, cmd);
    }

    closedir(d);

    return 0;
}
