#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int service_running(char *service) {

    FILE *f = fopen("/run/services.pid", "r");

    if (!f)
        return 0;

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        if (strcmp(line, service) == 0) {

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

void register_service(char *service) {

    FILE *f = fopen("/run/services.pid", "a");

    if (!f)
        return;

    fprintf(f, "%s\n", service);

    fclose(f);
}

int main() {

    printf("Bharat Service Manager Started\n");

    while (1) {

        FILE *f = fopen("/etc/services/enabled", "r");

        if (f) {

            char line[256];

            while (fgets(line, sizeof(line), f)) {

                line[strcspn(line, "\n")] = 0;

                if (strlen(line) == 0)
                    continue;

                if (service_running(line))
                    continue;

                pid_t pid = fork();

                if (pid == 0) {

                    execl(line, line, NULL);

                    exit(1);
                }

                register_service(line);
            }

            fclose(f);
        }

        sleep(60);
    }

    return 0;
}
