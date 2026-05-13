#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: sudo <command>\n");

        return 1;
    }

    char password[64];

    printf("sudo password: ");

    fgets(password, sizeof(password), stdin);

    password[strcspn(password, "\n")] = 0;

    FILE *f = fopen("/etc/passwd", "r");

    if (!f) {

        printf("Cannot open passwd database\n");

        return 1;
    }

    char line[128];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        char *user = strtok(line, ":");

        char *pass = strtok(NULL, ":");

        if (!user || !pass)
            continue;

        if (strcmp(user, "root") == 0 &&
            strcmp(pass, password) == 0) {

            fclose(f);

            char *env[] = {

                "USER=root",
                "HOME=/home/root",
                "PATH=/bin:/usr/bin",
                NULL
            };

            execve(argv[1], &argv[1], env);

            printf("sudo exec failed\n");

            return 1;
        }
    }

    fclose(f);

    printf("sudo authentication failed\n");

    return 1;
}
