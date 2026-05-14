#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>

void cleanup(int sig) {

    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void print_prompt(char *ps1) {

    for (int i = 0; ps1[i]; i++) {

        if (ps1[i] == '\\' &&
            ps1[i + 1] == '0' &&
            ps1[i + 2] == '3' &&
            ps1[i + 3] == '3') {

            putchar(27);

            i += 3;
        }

        else {

            putchar(ps1[i]);
        }
    }

    putchar(' ');
}

void load_config() {

    char path[256];

    char *home = getenv("HOME");

    if (!home)
        return;

    snprintf(path, sizeof(path),
             "%s/.bharatrc",
             home);

    FILE *f = fopen(path, "r");

    if (!f)
        return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        if (strncmp(line, "export ", 7) == 0) {

            char *env = line + 7;

            char *eq = strchr(env, '=');

            if (!eq)
                continue;

            *eq = 0;

            setenv(env, eq + 1, 1);
        }
    }

    fclose(f);
}

void add_history(char *cmd) {

    char path[256];

    char *home = getenv("HOME");

    if (!home)
        home = "/";

    snprintf(path, sizeof(path),
             "%s/.history",
             home);

    FILE *f = fopen(path, "a");

    if (!f)
        return;

    fprintf(f, "%s\n", cmd);

    fclose(f);
}

void execute_simple(char *cmd) {

    char buffer[256];

    strcpy(buffer, cmd);

    char *argv[32];

    int argc = 0;

    char *token = strtok(buffer, " ");

    while (token && argc < 31) {

        argv[argc++] = token;

        token = strtok(NULL, " ");
    }

    argv[argc] = NULL;
    int redirect = 0;
char *outfile = NULL;

for (int i = 0; i < argc; i++) {

    if (strcmp(argv[i], ">") == 0) {

        redirect = 1;

        outfile = argv[i + 1];

        argv[i] = NULL;

        break;
    }
}

    if (argc == 0)
        return;

    if (strcmp(argv[0], "cd") == 0) {

        if (argc > 1)
            chdir(argv[1]);

        return;
    }

    if (strcmp(argv[0], "exit") == 0) {

        exit(0);
    }

    pid_t pid = fork();

    if (pid == 0) {
       if (redirect && outfile) {

    freopen(outfile, "w", stdout);
}

        execvp(argv[0], argv);

        printf("Command not found: %s\n", argv[0]);

        exit(1);
    }

    else {

        waitpid(pid, NULL, 0);
    }
}

void execute_pipe(char *left, char *right) {

    int fd[2];

    pipe(fd);

    pid_t p1 = fork();

    if (p1 == 0) {

        dup2(fd[1], STDOUT_FILENO);

        close(fd[0]);
        close(fd[1]);

        execute_simple(left);

        exit(0);
    }

    pid_t p2 = fork();

    if (p2 == 0) {

        dup2(fd[0], STDIN_FILENO);

        close(fd[1]);
        close(fd[0]);

        execute_simple(right);

        exit(0);
    }

    close(fd[0]);
    close(fd[1]);

    wait(NULL);
    wait(NULL);
}

int main() {

    signal(SIGCHLD, cleanup);

    setenv("PATH", "/bin:/usr/bin", 1);

    load_config();

    char cmd[256];

    while (1) {

        char *ps1 = getenv("PS1");

        if (!ps1)
            ps1 = "bharat-shell$";

        print_prompt(ps1);

        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "") == 0)
            continue;

        add_history(cmd);

        char *pipepos = strchr(cmd, '|');

        if (pipepos) {

            *pipepos = 0;

            char *right = pipepos + 1;

            while (*right == ' ')
                right++;

            execute_pipe(cmd, right);

            continue;
        }

        execute_simple(cmd);
    }

    return 0;
}
