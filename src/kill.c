/*
 * kill <pid>        ask a process to stop (SIGTERM)
 * kill -9 <pid>     force it to stop (SIGKILL)
 *
 * The kernel checks the permission: you may only signal your own
 * processes, root may signal any.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

int main(int argc, char *argv[]) {

    int sig = SIGTERM;
    int at = 1;

    if (argc > 2 && argv[1][0] == '-') {

        sig = atoi(argv[1] + 1);
        at = 2;
    }

    if (at >= argc || sig <= 0 || sig > 64) {

        printf("usage: kill [-signal] <pid>\n");

        return 1;
    }

    int pid = atoi(argv[at]);

    if (pid <= 0) {

        printf("kill: invalid pid: %s\n", argv[at]);

        return 1;
    }

    if (kill(pid, sig) != 0) {

        printf("kill: %d: %s\n", pid, strerror(errno));

        return 1;
    }

    return 0;
}
