#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_VARS 32
#define MAX_FUNCTIONS 16
#define MAX_LINES 64

struct Variable {

    char name[64];
    char value[128];
};

struct Function {

    char name[64];
    char lines[MAX_LINES][256];
    int line_count;
};

struct Variable vars[MAX_VARS];
struct Function funcs[MAX_FUNCTIONS];

int var_count = 0;
int func_count = 0;

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

    strcpy(vars[var_count].name, name);
    strcpy(vars[var_count].value, value);

    var_count++;
}

struct Function* find_function(char *name) {

    for (int i = 0; i < func_count; i++) {

        if (strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    }

    return NULL;
}

void execute_command(char *cmd);

void execute_function(char *name) {

    struct Function *f = find_function(name);

    if (!f)
        return;

    for (int i = 0; i < f->line_count; i++) {

        execute_command(f->lines[i]);
    }
}

void execute_command(char *cmd) {

    cmd[strcspn(cmd, "\n")] = 0;

    if (strcmp(cmd, "") == 0)
        return;

    char buffer[256];

    strcpy(buffer, cmd);

    char *argv[32];

    int argc = 0;

    char *token = strtok(buffer, " ");

    while (token && argc < 31) {

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

        if (argc >= 3)
            set_var(argv[1], argv[2]);

        return;
    }

    if (strcmp(argv[0], "cd") == 0) {

        if (argc >= 2)
            chdir(argv[1]);

        return;
    }

    struct Function *f = find_function(argv[0]);

    if (f) {

        execute_function(argv[0]);

        return;
    }

    int background = 0;

    if (strcmp(argv[argc - 1], "&") == 0) {

        background = 1;

        argv[argc - 1] = NULL;
    }

    pid_t pid = fork();

    if (pid == 0) {

        for (int i = 0; argv[i] != NULL; i++) {

            if (strcmp(argv[i], ">") == 0) {

                argv[i] = NULL;

                FILE *f = fopen(argv[i + 1], "w");

                dup2(fileno(f), 1);

                fclose(f);

                break;
            }

            if (strcmp(argv[i], ">>") == 0) {

                argv[i] = NULL;

                FILE *f = fopen(argv[i + 1], "a");

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
    }
}

void execute_script(char *filename) {

    FILE *script = fopen(filename, "r");

    if (!script)
        return;

    char line[256];

    int condition = 1;

    while (fgets(line, sizeof(line), script)) {

        line[strcspn(line, "\n")] = 0;

        if (strncmp(line, "if EXIST ", 9) == 0) {

            char *file = line + 9;

            if (access(file, F_OK) == 0)
                condition = 1;
            else
                condition = 0;

            continue;
        }

        if (strcmp(line, "else") == 0) {

            condition = !condition;

            continue;
        }

        if (strcmp(line, "endif") == 0) {

            condition = 1;

            continue;
        }

        if (strncmp(line, "loop ", 5) == 0) {

            int count = atoi(line + 5);

            char loop_lines[MAX_LINES][256];

            int lc = 0;

            while (fgets(line, sizeof(line), script)) {

                line[strcspn(line, "\n")] = 0;

                if (strcmp(line, "endloop") == 0)
                    break;

                strcpy(loop_lines[lc++], line);
            }

            for (int i = 0; i < count; i++) {

                for (int j = 0; j < lc; j++) {

                    execute_command(loop_lines[j]);
                }
            }

            continue;
        }

        if (strncmp(line, "function ", 9) == 0) {

            struct Function *f = &funcs[func_count++];

            strcpy(f->name, line + 9);

            f->line_count = 0;

            while (fgets(line, sizeof(line), script)) {

                line[strcspn(line, "\n")] = 0;

                if (strcmp(line, "end") == 0)
                    break;

                strcpy(f->lines[f->line_count++], line);
            }

            continue;
        }

        if (condition)
            execute_command(line);
    }

    fclose(script);
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

        FILE *script = fopen(cmd, "rb");

        if (script) {

            char magic[4];

            fread(magic, 1, 4, script);

            fclose(script);

            if (magic[0] == 0x7f &&
                magic[1] == 'E' &&
                magic[2] == 'L' &&
                magic[3] == 'F') {

                execute_command(cmd);
            }

            else {

                execute_script(cmd);
            }

            continue;
        }

        execute_command(cmd);
    }

    return 0;
}
