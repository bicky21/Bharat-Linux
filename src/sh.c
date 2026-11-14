/*
 * Bharat shell
 *
 * Three ways to start it:
 *   sh              interactive: shows a prompt and reads the keyboard
 *   sh -c "cmd"     runs one command and exits (system() and popen() need this)
 *   sh script.sh    runs every line of a script file and exits
 *
 * Supported syntax:
 *   cmd arg "quoted arg"        run a program
 *   a | b | c                   pipes (any number of stages)
 *   cmd > f   cmd >> f   cmd < f   redirection
 *   cmd &                       run in the background
 *   NAME=value   export NAME=value   $NAME   $?
 *   cd [dir]   exit [code]
 *   if exists FILE then CMD
 *   while N do CMD
 *   # comment
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#include "libproc.h"

#define MAX_LINE   512
#define MAX_TOKENS 64
#define MAX_VARS   64
#define MAX_STAGES 8

struct shell_var {
    char name[64];
    char value[128];
};

struct shell_var vars[MAX_VARS];
int var_count = 0;

int last_status = 0;
int interactive = 0;

void execute_command(char *cmd);

/* ---------- small helpers ---------- */

void print_prompt() {

    printf("\033[1;32mbharat-shell$\033[0m ");
    fflush(stdout);
}

void trim(char *s) {

    s[strcspn(s, "\r\n")] = 0;

    char *start = s;

    while (*start == ' ' || *start == '\t')
        start++;

    if (start != s)
        memmove(s, start, strlen(start) + 1);

    int len = strlen(s);

    while (len > 0 &&
          (s[len - 1] == ' ' ||
           s[len - 1] == '\t')) {

        s[len - 1] = 0;
        len--;
    }
}

char *current_user() {

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    return user;
}

/* Collect finished background jobs so they do not stay as zombies. */
void reap_background() {

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        unregister_process(pid);
}

/* ---------- variables ---------- */

void set_var(char *name,
             char *value) {

    for (int i = 0; i < var_count; i++) {

        if (strcmp(vars[i].name, name) == 0) {

            snprintf(vars[i].value,
                     sizeof(vars[i].value),
                     "%s",
                     value);
            return;
        }
    }

    if (var_count >= MAX_VARS) {

        fprintf(stderr, "Too many variables\n");
        return;
    }

    snprintf(vars[var_count].name,
             sizeof(vars[var_count].name),
             "%s",
             name);

    snprintf(vars[var_count].value,
             sizeof(vars[var_count].value),
             "%s",
             value);

    var_count++;
}

char *get_var(char *name) {

    for (int i = 0; i < var_count; i++) {

        if (strcmp(vars[i].name, name) == 0)
            return vars[i].value;
    }

    return getenv(name);
}

int valid_name(char *name) {

    if (!isalpha((unsigned char)name[0]) && name[0] != '_')
        return 0;

    for (int i = 1; name[i]; i++) {

        if (!isalnum((unsigned char)name[i]) && name[i] != '_')
            return 0;
    }

    return 1;
}

/* Replace $NAME and $? in cmd. Never writes past MAX_LINE. */
void expand_variables(char *cmd) {

    char result[MAX_LINE];
    int out = 0;
    int i = 0;

    while (cmd[i] && out < MAX_LINE - 1) {

        if (cmd[i] != '$') {

            result[out++] = cmd[i++];
            continue;
        }

        char number[16];
        char name[64];
        char *value = NULL;

        if (cmd[i + 1] == '?') {

            snprintf(number, sizeof(number), "%d", last_status);
            value = number;
            i += 2;
        }
        else if (isalpha((unsigned char)cmd[i + 1]) ||
                 cmd[i + 1] == '_') {

            int j = 0;

            i++;

            while ((isalnum((unsigned char)cmd[i]) || cmd[i] == '_') &&
                   j < 63)
                name[j++] = cmd[i++];

            name[j] = 0;
            value = get_var(name);
        }
        else {

            /* a lonely '$' is just a normal character */
            result[out++] = cmd[i++];
            continue;
        }

        if (value) {

            for (int k = 0; value[k] && out < MAX_LINE - 1; k++)
                result[out++] = value[k];
        }
    }

    result[out] = 0;
    strcpy(cmd, result);
}

/* ---------- aliases (stored by the alias command in ~/.aliases) ---------- */

void expand_alias(char *cmd) {

    char *home = getenv("HOME");

    if (!home)
        return;

    char path[256];

    snprintf(path, sizeof(path), "%s/.aliases", home);

    FILE *f = fopen(path, "r");

    if (!f)
        return;

    int word = strcspn(cmd, " \t");
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        char *space = strchr(line, ' ');

        if (!space)
            continue;

        *space = 0;

        if ((int)strlen(line) == word &&
            strncmp(line, cmd, word) == 0) {

            char result[MAX_LINE];

            snprintf(result,
                     sizeof(result),
                     "%s%s",
                     space + 1,
                     cmd + word);

            strcpy(cmd, result);
            break;
        }
    }

    fclose(f);
}

/* ---------- splitting a line into words ---------- */

/*
 * Cuts cmd into tokens. Text inside "double" or 'single' quotes stays
 * together. The operators | > >> < & always become their own token, even
 * without spaces around them. Operator tokens are flagged in is_op so that
 * a quoted ">" is not mistaken for a redirection.
 */
int tokenize(char *cmd,
             char **tokens,
             int *is_op,
             char *storage) {

    int count = 0;
    int i = 0;
    int out = 0;

    while (cmd[i] && count < MAX_TOKENS - 1) {

        while (cmd[i] == ' ' || cmd[i] == '\t')
            i++;

        if (!cmd[i])
            break;

        tokens[count] = &storage[out];
        is_op[count] = 0;

        if (cmd[i] == '|' || cmd[i] == '<' || cmd[i] == '&') {

            storage[out++] = cmd[i++];
            is_op[count] = 1;
        }
        else if (cmd[i] == '>') {

            storage[out++] = cmd[i++];

            if (cmd[i] == '>')
                storage[out++] = cmd[i++];

            is_op[count] = 1;
        }
        else {

            while (cmd[i] &&
                   cmd[i] != ' ' && cmd[i] != '\t' &&
                   cmd[i] != '|' && cmd[i] != '<' &&
                   cmd[i] != '>' && cmd[i] != '&') {

                if (cmd[i] == '"' || cmd[i] == '\'') {

                    char quote = cmd[i++];

                    while (cmd[i] && cmd[i] != quote)
                        storage[out++] = cmd[i++];

                    if (cmd[i] == quote)
                        i++;
                }
                else {

                    storage[out++] = cmd[i++];
                }
            }
        }

        storage[out++] = 0;
        count++;
    }

    tokens[count] = NULL;

    return count;
}

/* ---------- running programs ---------- */

struct stage {
    char *argv[MAX_TOKENS];
    int argc;
};

void run_pipeline(char *cmd) {

    char storage[MAX_LINE * 2];
    char *tokens[MAX_TOKENS];
    int is_op[MAX_TOKENS];

    int count = tokenize(cmd, tokens, is_op, storage);

    if (count == 0)
        return;

    struct stage stages[MAX_STAGES];
    int stage_count = 1;

    char *in_file = NULL;
    char *out_file = NULL;
    int append = 0;
    int background = 0;

    stages[0].argc = 0;

    for (int i = 0; i < count; i++) {

        struct stage *st = &stages[stage_count - 1];

        if (!is_op[i]) {

            st->argv[st->argc++] = tokens[i];
            continue;
        }

        if (strcmp(tokens[i], "|") == 0) {

            if (st->argc == 0 || stage_count >= MAX_STAGES) {

                fprintf(stderr, "Invalid pipe\n");
                last_status = 2;
                return;
            }

            stages[stage_count++].argc = 0;
        }
        else if (strcmp(tokens[i], "&") == 0) {

            background = 1;
        }
        else {

            /* > >> < all need a file name after them */
            if (i + 1 >= count || is_op[i + 1]) {

                fprintf(stderr, "Missing file name after %s\n", tokens[i]);
                last_status = 2;
                return;
            }

            if (tokens[i][0] == '<') {

                in_file = tokens[i + 1];
            }
            else {

                out_file = tokens[i + 1];
                append = (tokens[i][1] == '>');
            }

            i++;
        }
    }

    for (int s = 0; s < stage_count; s++) {

        if (stages[s].argc == 0) {

            fprintf(stderr, "Missing command\n");
            last_status = 2;
            return;
        }

        stages[s].argv[stages[s].argc] = NULL;
    }

    /* built-in commands: they must change the shell itself, so no fork */
    if (stage_count == 1) {

        char **argv = stages[0].argv;

        if (strcmp(argv[0], "cd") == 0) {

            char *dir = argv[1] ? argv[1] : getenv("HOME");

            if (!dir || chdir(dir) != 0) {

                printf("Directory not found\n");
                last_status = 1;
            }
            else {

                last_status = 0;
            }

            return;
        }

        if (strcmp(argv[0], "exit") == 0)
            exit(argv[1] ? atoi(argv[1]) : last_status);
    }

    pid_t pids[MAX_STAGES];
    int prev_read = -1;

    fflush(stdout);

    for (int s = 0; s < stage_count; s++) {

        int fd[2] = {-1, -1};

        if (s < stage_count - 1 && pipe(fd) != 0) {

            perror("pipe");
            last_status = 1;
            return;
        }

        pid_t pid = fork();

        if (pid < 0) {

            perror("fork");
            last_status = 1;
            return;
        }

        if (pid == 0) {

            signal(SIGINT, SIG_DFL);

            if (prev_read >= 0) {

                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            else if (in_file) {

                int in = open(in_file, O_RDONLY);

                if (in < 0) {

                    fprintf(stderr, "Cannot open %s\n", in_file);
                    _exit(1);
                }

                dup2(in, STDIN_FILENO);
                close(in);
            }
            else if (background) {

                /* a background job must not steal keyboard input */
                int null = open("/dev/null", O_RDONLY);

                if (null >= 0) {

                    dup2(null, STDIN_FILENO);
                    close(null);
                }
            }

            if (s < stage_count - 1) {

                dup2(fd[1], STDOUT_FILENO);
                close(fd[0]);
                close(fd[1]);
            }
            else if (out_file) {

                int flags = O_WRONLY | O_CREAT |
                            (append ? O_APPEND : O_TRUNC);

                int out = open(out_file, flags, 0644);

                if (out < 0) {

                    fprintf(stderr, "Cannot write %s\n", out_file);
                    _exit(1);
                }

                dup2(out, STDOUT_FILENO);
                close(out);
            }

            execvp(stages[s].argv[0], stages[s].argv);

            fprintf(stderr,
                    "Command not found: %s\n",
                    stages[s].argv[0]);

            _exit(127);
        }

        pids[s] = pid;

        register_process(pid, current_user(), cmd);

        if (prev_read >= 0)
            close(prev_read);

        if (s < stage_count - 1) {

            close(fd[1]);
            prev_read = fd[0];
        }
    }

    if (background) {

        FILE *jf = fopen("/tmp/jobs", "a");

        if (jf) {

            fprintf(jf,
                    "[%d] running %s\n",
                    pids[stage_count - 1],
                    cmd);

            fclose(jf);
        }

        printf("[background pid %d]\n",
               pids[stage_count - 1]);

        last_status = 0;

        return;
    }

    for (int s = 0; s < stage_count; s++) {

        int status = 0;

        if (waitpid(pids[s], &status, 0) > 0 &&
            s == stage_count - 1) {

            if (WIFEXITED(status))
                last_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                last_status = 128 + WTERMSIG(status);
        }

        unregister_process(pids[s]);
    }
}

/* ---------- if / while ---------- */

void execute_if(char *cmd) {

    char file[128];
    char action[MAX_LINE];

    if (sscanf(cmd,
               "if exists %127s then %511[^\n]",
               file,
               action) != 2) {

        printf("Invalid if syntax (use: if exists FILE then COMMAND)\n");
        last_status = 2;
        return;
    }

    if (access(file, F_OK) == 0)
        execute_command(action);
}

void execute_while(char *cmd) {

    int count;
    char action[MAX_LINE];

    if (sscanf(cmd,
               "while %d do %511[^\n]",
               &count,
               action) != 2) {

        printf("Invalid while syntax (use: while N do COMMAND)\n");
        last_status = 2;
        return;
    }

    for (int i = 0; i < count; i++) {

        char copy[MAX_LINE];

        strcpy(copy, action);
        execute_command(copy);
    }
}

/* ---------- one full command line ---------- */

/* cmd must be a writable buffer of at least MAX_LINE bytes. */
void execute_command(char *cmd) {

    trim(cmd);

    if (cmd[0] == 0 || cmd[0] == '#')
        return;

    expand_alias(cmd);
    expand_variables(cmd);
    trim(cmd);

    if (cmd[0] == 0)
        return;

    if (strncmp(cmd, "if ", 3) == 0) {

        execute_if(cmd);
        return;
    }

    if (strncmp(cmd, "while ", 6) == 0) {

        execute_while(cmd);
        return;
    }

    if (strncmp(cmd, "export ", 7) == 0) {

        char *name = cmd + 7;
        char *eq = strchr(name, '=');

        trim(name);

        if (eq) {

            *eq = 0;

            if (valid_name(name))
                setenv(name, eq + 1, 1);
            else
                printf("Invalid variable name\n");
        }
        else {

            char *value = get_var(name);

            if (value)
                setenv(name, value, 1);
        }

        last_status = 0;
        return;
    }

    /* NAME=value (one word, and the part before '=' is a valid name) */
    char *eq = strchr(cmd, '=');

    if (eq && !strpbrk(cmd, " \t")) {

        *eq = 0;

        if (valid_name(cmd)) {

            set_var(cmd, eq + 1);
            last_status = 0;
            return;
        }

        *eq = '=';
    }

    run_pipeline(cmd);
}

/* ---------- reading commands ---------- */

void save_history(char *cmd) {

    char *home = getenv("HOME");

    if (!home)
        return;

    char path[256];

    snprintf(path, sizeof(path), "%s/.history", home);

    FILE *f = fopen(path, "a");

    if (!f)
        return;

    fprintf(f, "%s\n", cmd);
    fclose(f);
}

void run_stream(FILE *in,
                int prompt) {

    char cmd[MAX_LINE];

    while (1) {

        reap_background();

        if (prompt)
            print_prompt();

        /* NULL means end of input (Ctrl-D or end of script): stop */
        if (!fgets(cmd, sizeof(cmd), in))
            break;

        trim(cmd);

        if (cmd[0] == 0)
            continue;

        if (prompt)
            save_history(cmd);

        execute_command(cmd);
    }
}

void run_file(char *path,
              int must_exist) {

    FILE *f = fopen(path, "r");

    if (!f) {

        if (must_exist) {

            fprintf(stderr, "Cannot open script: %s\n", path);
            exit(127);
        }

        return;
    }

    run_stream(f, 0);
    fclose(f);
}

int main(int argc, char *argv[]) {

    if (!getenv("PATH"))
        setenv("PATH", "/bin:/usr/bin", 1);

    /* sh -c "command"   (the C library's system() sends: sh -c -- "command") */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {

        char cmd[MAX_LINE];
        int at = 2;

        if (strcmp(argv[at], "--") == 0 && argc > at + 1)
            at++;

        snprintf(cmd, sizeof(cmd), "%s", argv[at]);
        execute_command(cmd);

        return last_status;
    }

    /* sh script.sh */
    if (argc >= 2) {

        run_file(argv[1], 1);

        return last_status;
    }

    interactive = isatty(STDIN_FILENO);

    if (interactive) {

        /* Ctrl-C should stop the running program, not the shell */
        signal(SIGINT, SIG_IGN);

        char *home = getenv("HOME");

        if (home) {

            char rc[256];

            snprintf(rc, sizeof(rc), "%s/.bharatrc", home);
            run_file(rc, 0);
        }
    }

    run_stream(stdin, interactive);

    if (interactive)
        printf("\n");

    return last_status;
}
