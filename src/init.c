#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main() {

    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/run", 0755);

    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    FILE *lf = fopen("/var/log/bharat.log", "a");

    if (lf) {

        fprintf(lf, "INIT STARTED\n");

        fclose(lf);
    }

    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);

    printf("\n");
    printf("=================================\n");
    printf("        Bharat-linux CLI        \n");
    printf("=================================\n");

    printf("Launching login...\n");

    while (1) {

        pid_t pid = fork();

        if (pid == 0) {

            char *args[] = {"/bin/login", NULL};

            execv("/bin/login", args);

            printf("LOGIN EXEC FAILED\n");

            while (1);
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
