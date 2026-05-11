#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>

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

        pid_t pid = fork();

        if (pid == 0) {

            char *args[] = {"/bin/sh", NULL};

            execve("/bin/sh", args, NULL);

            printf("Failed to launch shell\n");

            return 1;
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
