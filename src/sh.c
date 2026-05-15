#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>

struct shell_var {

    char name[64];
    char value[128];
};

struct shell_var vars[64];

int var_count = 0;

void print_prompt() {

    printf("\033[1;32mbharat-shell$\033[0m ");

    fflush(stdout);
}

void normalize(char *cmd) {

    char temp[512] = {0};

    int j = 0;

    for (int i = 0; cmd[i]; i++) {

        if (cmd[i] == '>') {

            temp[j++] = ' ';

            temp[j++] = '>';

            if (cmd[i + 1] == '>') {

                temp[j++] = '>';

                i++;
            }

            temp[j++] = ' ';
        }

        else if (cmd[i] == '|') {

            temp[j++] = ' ';

            temp[j++] = '|';

            temp[j++] = ' ';
        }

        else {

            temp[j++] = cmd[i];
        }
    }

    temp[j] = 0;

    strcpy(cmd, temp);
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

char *get_var(char *name) {

    for (int i = 0; i < var_count; i++) {

        if (strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    }

    return getenv(name);
}

void expand_variables(char *cmd) {

    char result[512] = {0};

    int i = 0;

    while (cmd[i]) {

        if (cmd[i] == '$') {

            i++;

            char var[64] = {0};

            int j = 0;

            while (cmd[i] &&
                   cmd[i] != ' ' &&
                   j < 63) {

                var[j++] = cmd[i++];
            }

            char *value = get_var(var);

            if (value)
                strcat(result, value);
        }

        else {

            int len = strlen(result);

            result[len] = cmd[i];

            result[len + 1] = 0;

            i++;
        }
    }

    strcpy(cmd, result);
}

int file_exists(char *path) {

    return access(path, F_OK) == 0;
}

void execute_command(char *cmd);

void execute_if(char *cmd) {

    char file[128];
    char action[256];

    if (sscanf(cmd,
               "if exists %127s then %255[^\n]",
               file,
               action) != 2) {

        printf("Invalid if syntax\n");

        return;
    }

    if (file_exists(file)) {

        execute_command(action);
    }
}

void execute_while(char *cmd) {

    int count;

    char action[256];

    if (sscanf(cmd,
               "while %d do %255[^\n]",
               &count,
               action) != 2) {

        printf("Invalid while syntax\n");

        return;
    }

    for (int i = 0; i < count; i++) {

        execute_command(action);
    }
}

void execute_simple(char *cmd) {

    expand_variables(cmd);

    if (strchr(cmd, '=') &&
        strncmp(cmd, "export ", 7) != 0) {

        char *eq = strchr(cmd, '=');

        *eq = 0;

        set_var(cmd, eq + 1);

        return;
    }

    if (strncmp(cmd, "export ", 7) == 0) {

        char *env = cmd + 7;

        char *eq = strchr(env, '=');

        if (!eq)
            return;

        *eq = 0;

        setenv(env, eq + 1, 1);

        return;
    }

    char buffer[512];

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

    char *redirect;

    if (append)
        redirect = strstr(cmd, ">>");
    else
        redirect = strchr(cmd, '>');

    if (!redirect)
        return;

    *redirect = 0;

    if (append)
        redirect += 2;
    else
        redirect += 1;

    while (*redirect == ' ')
        redirect++;

    char filename[128];

    strcpy(filename, redirect);

    char command[256];

    strcpy(command, cmd);

    char *argv[32];

    parse_args(command, argv);

    pid_t pid = fork();

    if (pid == 0) {

        int fd;

        if (append) {

            fd = open(filename,
                      O_WRONLY | O_CREAT | O_APPEND,
                      0644);
        }

        else {

            fd = open(filename,
                      O_WRONLY | O_CREAT | O_TRUNC,
                      0644);
        }

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

void execute_command(char *cmd) {

    normalize(cmd);

    if (strncmp(cmd, "if ", 3) == 0) {

        execute_if(cmd);

        return;
    }

    if (strncmp(cmd, "while ", 6) == 0) {

        execute_while(cmd);

        return;
    }

    if (strstr(cmd, ">>")) {

        execute_redirect(cmd, 1);

        return;
    }

    if (strchr(cmd, '>')) {

        execute_redirect(cmd, 0);

        return;
    }

    if (strchr(cmd, '|')) {

        execute_pipe(cmd);

        return;
    }

    execute_simple(cmd);
}

int main() {

    setenv("PATH", "/bin:/usr/bin", 1);

    char cmd[512];

    while (1) {

        print_prompt();

        if (!fgets(cmd, sizeof(cmd), stdin))
            continue;

        cmd[strcspn(cmd, "\n")] = 0;

        if (strcmp(cmd, "") == 0)
            continue;

        if (strcmp(cmd, "exit") == 0)
            break;

        execute_command(cmd);
    }

    return 0;
}
