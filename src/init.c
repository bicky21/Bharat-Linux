#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main() {

    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/run", 0755);

    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);

    printf("\n");
    printf("=================================\n");
    printf("        Bharat-linux CLI        \n");
    printf("=================================\n");

    printf("Launching login...\n");

    pid_t svc = fork();

    if (svc == 0) {

        execl("/bin/serviced",
              "/bin/serviced",
              NULL);

        exit(1);
    }

    while (1) {

        pid_t pid = fork();

        if (pid == 0) {

            char *args[] = {"/bin/login", NULL};

            execv("/bin/login", args);

            exit(1);
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
