#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

void trim(char *s) {

    s[strcspn(s, "\n")] = 0;

    s[strcspn(s, "\r")] = 0;
}

void disable_echo() {

    struct termios t;

    tcgetattr(STDIN_FILENO, &t);

    t.c_lflag &= ~ECHO;

    tcsetattr(STDIN_FILENO,
              TCSANOW,
              &t);
}

void enable_echo() {

    struct termios t;

    tcgetattr(STDIN_FILENO, &t);

    t.c_lflag |= ECHO;

    tcsetattr(STDIN_FILENO,
              TCSANOW,
              &t);
}

int authenticate(char *user,
                 char *pass) {

    FILE *f = fopen("/etc/passwd", "r");

    if (!f) {

        printf("Cannot open passwd database\n");

        return 0;
    }

    char line[512];

    while (fgets(line,
                 sizeof(line),
                 f)) {

        trim(line);

        char *username = strtok(line, ":");
        char *password = strtok(NULL, ":");
        char *uid      = strtok(NULL, ":");
        char *gid      = strtok(NULL, ":");
        char *home     = strtok(NULL, ":");
        char *shell    = strtok(NULL, ":");

        if (!username ||
            !password ||
            !uid ||
            !gid ||
            !home ||
            !shell)
            continue;

        trim(username);
        trim(password);

        if (strcmp(user, username) == 0 &&
            strcmp(pass, password) == 0) {

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

void load_user_environment(char *user) {

    FILE *f = fopen("/etc/passwd", "r");

    if (!f)
        return;

    char line[512];

    while (fgets(line,
                 sizeof(line),
                 f)) {

        trim(line);

        char *username = strtok(line, ":");
        char *password = strtok(NULL, ":");
        char *uid      = strtok(NULL, ":");
        char *gid      = strtok(NULL, ":");
        char *home     = strtok(NULL, ":");
        char *shell    = strtok(NULL, ":");

        if (!username ||
            !password ||
            !uid ||
            !gid ||
            !home ||
            !shell)
            continue;

        if (strcmp(user, username) == 0) {

            setenv("USER",
                   username,
                   1);

            setenv("HOME",
                   home,
                   1);

            chdir(home);

            fclose(f);

            return;
        }
    }

    fclose(f);
}

int main() {

    char user[64];

    char pass[64];

    while (1) {

        printf("login: ");

        fflush(stdout);

        if (!fgets(user,
                   sizeof(user),
                   stdin))
            continue;

        trim(user);

        printf("password: ");

        fflush(stdout);

        disable_echo();

        if (!fgets(pass,
                   sizeof(pass),
                   stdin)) {

            enable_echo();

            continue;
        }

        enable_echo();

        printf("\n");

        trim(pass);

        if (authenticate(user,
                         pass)) {

            load_user_environment(user);

            printf("Welcome %s\n",
                   user);

            execl("/bin/sh",
                  "/bin/sh",
                  NULL);

            perror("shell failed");

            exit(1);
        }

        printf("Login failed\n");
    }

    return 0;
}
