#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "libproc.h"

int main(int argc, char *argv[]) {

    if (argc != 2) {

        printf("usage: kill <pid>\n");

        return 1;
    }

    int pid = atoi(argv[1]);

    char owner[64];

    if (!process_owner(pid, owner)) {

        printf("Unknown process\n");

        return 1;
    }

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    if (strcmp(user, "root") != 0 &&
        strcmp(user, owner) != 0) {

        printf("Permission denied\n");

        return 1;
    }

    if (kill(pid, SIGKILL) != 0) {

        printf("Kill failed\n");

        return 1;
    }

    unregister_process(pid);

    printf("Process killed\n");

    return 0;
}
