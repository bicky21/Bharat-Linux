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

        pid_t pid = fork();

        if (pid == 0) {

            char path[300];

            snprintf(path, sizeof(path), "/bin/%s", cmd);

            char *args[] = {path, NULL};

            execve(path, args, NULL);

            printf("Command not found: %s\n", cmd);

            exit(1);
        }

        else {

            wait(NULL);
        }
    }

    return 0;
}
