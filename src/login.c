#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main() {

    char username[64];
    char password[64];

    printf("login: ");

    fgets(username, sizeof(username), stdin);

    username[strcspn(username, "\n")] = 0;

    printf("password: ");

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

        if (strcmp(user, username) == 0 &&
            strcmp(pass, password) == 0) {

            setenv("USER", username, 1);

            char home[128];

            snprintf(home, sizeof(home),
                     "/home/%s",
                     username);

            setenv("HOME", home, 1);

            chdir(home);

            printf("Welcome %s\n", username);

            char *args[] = {"/bin/sh", NULL};

            execv("/bin/sh", args);

            printf("Shell launch failed\n");

            return 1;
        }
    }

    fclose(f);

    printf("Login failed\n");

    return 1;
}
