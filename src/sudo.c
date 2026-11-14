#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: sudo <command>\n");

        return 1;
    }

    char password[64];

    printf("sudo password: ");
    fflush(stdout);

    /* hide the password while it is typed */
    struct termios old, hidden;
    int has_terminal = tcgetattr(STDIN_FILENO, &old) == 0;

    if (has_terminal) {

        hidden = old;
        hidden.c_lflag &= ~ECHO;

        tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    }

    char *typed = fgets(password, sizeof(password), stdin);

    if (has_terminal) {

        tcsetattr(STDIN_FILENO, TCSANOW, &old);

        printf("\n");
    }

    if (!typed)
        return 1;

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

            setenv("USER", "root", 1);
            setenv("LOGNAME", "root", 1);
            setenv("HOME", "/home/root", 1);
            setenv("PATH", "/bin:/usr/bin", 1);

            /* execvp searches PATH, so "sudo ls" works like "sudo /bin/ls" */
            execvp(argv[1], &argv[1]);

            printf("sudo exec failed\n");

            return 1;
        }
    }

    fclose(f);

    printf("sudo authentication failed\n");

    return 1;
}
