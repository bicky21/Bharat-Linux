#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

void start_services() {

    FILE *f = fopen("/etc/services/enabled", "r");

    if (!f)
        return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        if (strlen(line) == 0)
            continue;

        pid_t pid = fork();

        if (pid == 0) {

            execl(line, line, NULL);

            exit(1);
        }
    }

    fclose(f);
}

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

    start_services();

    while (1) {

        pid_t pid = fork();

        if (pid == 0) {

            char *args[] = {"/bin/login", NULL};

            execv("/bin/login", args);

            while (1);
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
