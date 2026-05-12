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

            if (chdir(path) != 0)
                printf("cd failed\n");

            continue;
        }

        char *pipe_pos = strchr(cmd, '|');

        if (pipe_pos != NULL) {

            *pipe_pos = '\0';

            char *cmd1 = cmd;
            char *cmd2 = pipe_pos + 1;

            while (*cmd2 == ' ')
                cmd2++;

            int fd[2];

            pipe(fd);

            pid_t p1 = fork();

            if (p1 == 0) {

                dup2(fd[1], 1);

                close(fd[0]);
                close(fd[1]);

                char *argv1[16];
                int argc1 = 0;

                char *token = strtok(cmd1, " ");

                while (token && argc1 < 15) {

                    argv1[argc1++] = token;
                    token = strtok(NULL, " ");
                }

                argv1[argc1] = NULL;

                char path[256];

                snprintf(path, sizeof(path), "/bin/%s", argv1[0]);

                execve(path, argv1, NULL);

                exit(1);
            }

            pid_t p2 = fork();

            if (p2 == 0) {

                dup2(fd[0], 0);

                close(fd[0]);
                close(fd[1]);

                char *argv2[16];
                int argc2 = 0;

                char *token = strtok(cmd2, " ");

                while (token && argc2 < 15) {

                    argv2[argc2++] = token;
                    token = strtok(NULL, " ");
                }

                argv2[argc2] = NULL;

                char path[256];

                snprintf(path, sizeof(path), "/bin/%s", argv2[0]);

                execve(path, argv2, NULL);

                exit(1);
            }

            close(fd[0]);
            close(fd[1]);

            wait(NULL);
            wait(NULL);

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

        int background = 0;

        if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {

            background = 1;

            argv[argc - 1] = NULL;

            argc--;
        }

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

            if (!background) {

                wait(NULL);
            }

            else {

                printf("[background pid %d]\n", pid);
            }
        }
    }

    return 0;
}
