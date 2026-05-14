#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: kill <pid>\n");

        return 1;
    }

    int pid = atoi(argv[1]);

    if (kill(pid, SIGTERM) == 0) {

        printf("Killed %d\n", pid);
    }

    else {

        printf("Kill failed\n");
    }

    return 0;
}
