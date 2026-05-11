#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

int main() {

    char cmd[256];

    while (1) {

        printf("bharat-shell$ ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "") == 0)
            continue;

        if (strcmp(cmd, "exit") == 0)
            break;

        if (strncmp(cmd, "cd ", 3) == 0) {

            char *path = cmd + 3;

            if (chdir(path) != 0) {
                printf("cd failed\n");
            }

            continue;
        }

        char *argv[16];

        int argc = 0;

        char *token = strtok(cmd, " ");

        while (token != NULL && argc < 15) {

            argv[argc++] = token;

            token = strtok(NULL, " ");
        }

        argv[argc] = NULL;

        pid_t pid = fork();

        if (pid == 0) {

            for (int i = 0; argv[i] != NULL; i++) {

                if (strcmp(argv[i], ">") == 0) {

                    argv[i] = NULL;

                    FILE *f = fopen(argv[i + 1], "w");

                    if (!f) {
                        printf("redirection failed\n");
                        exit(1);
                    }

                    dup2(fileno(f), 1);

                    fclose(f);

                    break;
                }
            }

            char path[256];

            snprintf(path, sizeof(path), "/bin/%s", argv[0]);

            execve(path, argv, NULL);

            printf("Command not found: %s\n", argv[0]);

            exit(1);
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
