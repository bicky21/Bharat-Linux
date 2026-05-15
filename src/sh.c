#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>

void print_prompt() {

    printf("\033[1;32mbharat-shell$\033[0m ");

    fflush(stdout);
}

void parse_args(char *cmd, char **argv) {

    int argc = 0;

    char *token = strtok(cmd, " ");

    while (token && argc < 31) {

        argv[argc++] = token;

        token = strtok(NULL, " ");
    }

    argv[argc] = NULL;
}

void execute_simple(char *cmd) {

    char buffer[256];

    strcpy(buffer, cmd);

    char *argv[32];

    parse_args(buffer, argv);

    if (!argv[0])
        return;

    if (strcmp(argv[0], "cd") == 0) {

        if (argv[1])
            chdir(argv[1]);

        return;
    }

    pid_t pid = fork();

    if (pid == 0) {

        execvp(argv[0], argv);

        printf("Command not found: %s\n", argv[0]);

        exit(1);
    }

    waitpid(pid, NULL, 0);
}

void execute_redirect(char *cmd, int append) {

    char *file;

    if (append)
        file = strstr(cmd, ">>");
    else
        file = strchr(cmd, '>');

    if (!file)
        return;

    *file = 0;

    if (append)
        file += 2;
    else
        file += 1;

    while (*file == ' ')
        file++;

    char buffer[256];

    strcpy(buffer, cmd);

    char *argv[32];

    parse_args(buffer, argv);

    pid_t pid = fork();

    if (pid == 0) {

        int fd;

        if (append)
            fd = open(file,
                      O_WRONLY | O_CREAT | O_APPEND,
                      0644);
        else
            fd = open(file,
                      O_WRONLY | O_CREAT | O_TRUNC,
                      0644);

        dup2(fd, STDOUT_FILENO);

        close(fd);

        execvp(argv[0], argv);

        exit(1);
    }

    waitpid(pid, NULL, 0);
}

void execute_pipe(char *cmd) {

    char *pipe_pos = strchr(cmd, '|');

    if (!pipe_pos)
        return;

    *pipe_pos = 0;

    char *cmd2 = pipe_pos + 1;

    while (*cmd2 == ' ')
        cmd2++;

    char left[256];
    char right[256];

    strcpy(left, cmd);
    strcpy(right, cmd2);

    char *argv1[32];
    char *argv2[32];

    parse_args(left, argv1);
    parse_args(right, argv2);

    int fd[2];

    pipe(fd);

    pid_t p1 = fork();

    if (p1 == 0) {

        dup2(fd[1], STDOUT_FILENO);

        close(fd[0]);
        close(fd[1]);

        execvp(argv1[0], argv1);

        exit(1);
    }

    pid_t p2 = fork();

    if (p2 == 0) {

        dup2(fd[0], STDIN_FILENO);

        close(fd[0]);
        close(fd[1]);

        execvp(argv2[0], argv2);

        exit(1);
    }

    close(fd[0]);
    close(fd[1]);

    waitpid(p1, NULL, 0);
    waitpid(p2, NULL, 0);
}

int main() {

    setenv("PATH", "/bin:/usr/bin", 1);

    char cmd[256];

    while (1) {

        print_prompt();

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "") == 0)
            continue;

        if (strcmp(cmd, "exit") == 0)
            break;

        if (strstr(cmd, ">>")) {

            execute_redirect(cmd, 1);

            continue;
        }

        if (strchr(cmd, '>')) {

            execute_redirect(cmd, 0);

            continue;
        }

        if (strchr(cmd, '|')) {

            execute_pipe(cmd);

            continue;
        }

        execute_simple(cmd);
    }

    return 0;
}
