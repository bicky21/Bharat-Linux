#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: kill <pid>\n");

        return 1;
    }

    int pid = atoi(argv[1]);

    if (pid <= 0) {

        printf("Invalid pid\n");

        return 1;
    }

    if (kill(pid, SIGKILL) == 0)
        printf("Process killed\n");
    else
        printf("Kill failed\n");

    return 0;
}
