#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_VARS 32

struct Variable {

    char name[64];
    char value[128];
};

struct Variable vars[MAX_VARS];

int var_count = 0;

void cleanup(int sig) {

    while (waitpid(-1, NULL, WNOHANG) > 0);
}

char* get_var(char *name) {

    for (int i = 0; i < var_count; i++) {

        if (strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    }

    return NULL;
}

void set_var(char *name, char *value) {

    for (int i = 0; i < var_count; i++) {

        if (strcmp(vars[i].name, name) == 0) {

            strcpy(vars[i].value, value);

            return;
        }
    }

    if (var_count < MAX_VARS) {

        strcpy(vars[var_count].name, name);

        strcpy(vars[var_count].value, value);

        var_count++;
    }
}

void execute_command(char *cmd) {

    cmd[strcspn(cmd, "\n")] = 0;

    if (strcmp(cmd, "") == 0)
        return;

    char *argv[16];

    int argc = 0;

    char *token = strtok(cmd, " ");

    while (token != NULL && argc < 15) {

        if (token[0] == '$') {

            char *value = get_var(token + 1);

            if (value)
                token = value;
        }

        argv[argc++] = token;

        token = strtok(NULL, " ");
    }

    argv[argc] = NULL;

    if (argc == 0)
        return;

    if (strcmp(argv[0], "set") == 0) {

        if (argc >= 3) {

            set_var(argv[1], argv[2]);
        }

        return;
    }

    if (strcmp(argv[0], "cd") == 0) {

        if (argc > 1) {

            if (chdir(argv[1]) != 0)
                printf("cd failed\n");
        }

        return;
    }

    int background = 0;

    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {

        background = 1;

        argv[argc - 1] = NULL;
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

        execvp(argv[0], argv);

        printf("Command not found: %s\n", argv[0]);

        exit(1);
    }

    else {

        if (!background)
            wait(NULL);
        else
            printf("[background pid %d]\n", pid);
    }
}

int main() {

    signal(SIGCHLD, cleanup);

    setenv("PATH", "/bin:/usr/bin", 1);

    char cmd[256];

    while (1) {

        printf("bharat-shell$ ");
        fflush(stdout);

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "exit") == 0)
            break;

        FILE *script = fopen(cmd, "r");

        if (script != NULL) {

            char line[256];

            while (fgets(line, sizeof(line), script)) {

                execute_command(line);
            }

            fclose(script);

            continue;
        }

        execute_command(cmd);
    }

    return 0;
}
