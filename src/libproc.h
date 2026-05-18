#ifndef LIBPROC_H
#define LIBPROC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void register_process(pid_t pid,
                      const char *user,
                      const char *cmd) {

    FILE *f = fopen("/run/processes.db", "a");

    if (!f)
        return;

    fprintf(f,
            "%d:%s:%s\n",
            pid,
            user,
            cmd);

    fclose(f);
}

void unregister_process(pid_t pid) {

    FILE *f = fopen("/run/processes.db", "r");

    if (!f)
        return;

    FILE *tmp = fopen("/run/processes.tmp", "w");

    if (!tmp) {

        fclose(f);

        return;
    }

    char line[512];

    while (fgets(line,
                 sizeof(line),
                 f)) {

        int file_pid;

        sscanf(line,
               "%d:",
               &file_pid);

        if (file_pid != pid)
            fputs(line, tmp);
    }

    fclose(f);
    fclose(tmp);

    rename("/run/processes.tmp",
           "/run/processes.db");
}

int process_owner(pid_t pid,
                  char *owner) {

    FILE *f = fopen("/run/processes.db", "r");

    if (!f)
        return 0;

    char line[512];

    while (fgets(line,
                 sizeof(line),
                 f)) {

        int file_pid;

        char file_user[64];
        char cmd[256];

        if (sscanf(line,
                   "%d:%63[^:]:%255[^\n]",
                   &file_pid,
                   file_user,
                   cmd) != 3)
            continue;

        if (file_pid == pid) {

            strcpy(owner,
                   file_user);

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

#endif
