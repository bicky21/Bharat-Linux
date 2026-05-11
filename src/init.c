#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <sys/stat.h>
int main() {

    mkdir("/proc", 0555);
    mkdir("/sys", 0555);

    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);

    printf("\n");
    printf("=================================\n");
    printf("      Bharat-linux CLI v0.1     \n");
    printf("=================================\n");

    while (1) {

        char cmd[256];

        printf("bharat# ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        if (cmd[0] == '\n')
            continue;

        system(cmd);
    }

    return 0;
}
