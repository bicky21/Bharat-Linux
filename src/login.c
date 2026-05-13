#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void load_profile(char *user) {

    char path[128];

    snprintf(path, sizeof(path), "/home/%s/.profile", user);

    FILE *f = fopen(path, "r");

    if (!f)
        return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        system(line);
    }

    fclose(f);
}

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

            snprintf(home, sizeof(home), "/home/%s", username);

            setenv("HOME", home, 1);

            chdir(home);

            load_profile(username);

            printf("Welcome %s\n", username);

            execl("/bin/sh", "/bin/sh", NULL);

            return 0;
        }
    }

    fclose(f);

    printf("Login failed\n");

    return 1;
}
