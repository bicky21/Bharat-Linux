#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int main() {

    char cmd[256];

    while (1) {

        printf("bharat-shell$ ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "exit") == 0) {
            break;
        }

        else if (strcmp(cmd, "hello") == 0) {
            printf("Welcome to Bharat-linux\n");
        }

        else if (strcmp(cmd, "clear") == 0) {
            printf("\033[2J\033[H");
        }

        else if (strcmp(cmd, "") == 0) {
            continue;
        }

        else {
            printf("Unknown command: %s\n", cmd);
        }
    }

    return 0;
}
