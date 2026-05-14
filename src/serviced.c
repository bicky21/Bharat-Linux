#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

int main() {

    pid_t pid = fork();

    if (pid > 0)
        exit(0);

    setsid();

    int fd = open("/dev/null", O_RDWR);

    if (fd >= 0) {

        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);

        close(fd);
    }

    while (1) {

        FILE *f = fopen("/etc/services/enabled", "r");

        if (f) {

            char line[256];

            while (fgets(line, sizeof(line), f)) {

                line[strcspn(line, "\n")] = 0;

                if (strlen(line) == 0)
                    continue;

                pid_t svc = fork();

                if (svc == 0) {

                    execl(line, line, NULL);

                    exit(1);
                }
            }

            fclose(f);
        }

        sleep(300);
    }

    return 0;
}
