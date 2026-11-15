/*
 * The start of every command: find which one was asked for.
 */

#include <stdarg.h>
#include "coreutils.h"
#include "commands.h"

const char *prog = "coreutils";

int fail(const char *fmt, ...) {

    va_list ap;

    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    return 1;
}

int options(int argc, char **argv, const char *allowed, const char *with_value,
            int flags[128], char *values[128]) {

    int i = 1;

    for (; i < argc; i++) {

        char *a = argv[i];

        if (strcmp(a, "--") == 0)
            return i + 1;

        /* "-" alone means "the keyboard / standard input": not an option */
        if (a[0] != '-' || a[1] == 0)
            break;

        /* a negative number like "-5" for head/tail is handled by them */
        for (int k = 1; a[k]; k++) {

            unsigned char c = (unsigned char)a[k];

            if (c >= 128 || !strchr(allowed, c)) {

                fail("unknown option -%c", c);
                exit(2);
            }

            flags[c] = 1;

            if (with_value && strchr(with_value, c)) {

                /* "-n5" or "-n 5" */
                if (a[k + 1]) {

                    values[c] = &a[k + 1];
                }
                else if (i + 1 < argc) {

                    values[c] = argv[++i];
                }
                else {

                    fail("option -%c needs a value", c);
                    exit(2);
                }

                break;
            }
        }
    }

    return i;
}

void human(long long n, char *out, size_t size) {

    const char *units = "BKMGT";
    double v = (double)n;
    int u = 0;

    while (v >= 1024 && u < 4) {

        v /= 1024;
        u++;
    }

    if (u == 0)
        snprintf(out, size, "%lld", n);
    else
        snprintf(out, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
}

void join(const char *dir, const char *name, char *out, size_t size) {

    size_t n = strlen(dir);

    snprintf(out, size, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
}

int copy_fd(int in, int out) {

    char buf[65536];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) > 0) {

        char *p = buf;

        while (n > 0) {

            ssize_t w = write(out, p, (size_t)n);

            if (w < 0)
                return -1;

            p += w;
            n -= w;
        }
    }

    return n < 0 ? -1 : 0;
}

struct command {
    const char *name;
    int (*run)(int, char **);
    const char *help;
};

#define C(name, help) {#name, cmd_##name, help},

static const struct command commands[] = {
#include "list.h"
    {"[", cmd_bracket, "the same as test, ending with ]"},
};

#define COUNT ((int)(sizeof(commands) / sizeof(commands[0])))

static int list_commands(void) {

    printf("Bharat Linux commands (run any of them with --help):\n\n");

    for (int i = 0; i < COUNT; i++)
        printf("  %-10s %s\n", commands[i].name, commands[i].help);

    printf("\nAlso: sudo, passwd, useradd, userdel, usermod, id, groups, ps, kill,\n"
           "      top, nano, service, desktop, poweroff, reboot\n");

    return 0;
}

COMMAND(help) {

    (void)argc;
    (void)argv;

    return list_commands();
}

int main(int argc, char **argv) {

    const char *name = strrchr(argv[0], '/');

    name = name ? name + 1 : argv[0];

    /* "coreutils ls -l" works too */
    if (strcmp(name, "coreutils") == 0) {

        if (argc < 2)
            return list_commands();

        argv++;
        argc--;
        name = argv[0];
    }

    prog = name;

    for (int i = 0; i < COUNT; i++) {

        if (strcmp(commands[i].name, name) == 0) {

            if (argc > 1 && strcmp(argv[1], "--help") == 0) {

                printf("%s: %s\n", commands[i].name, commands[i].help);
                return 0;
            }

            return commands[i].run(argc, argv);
        }
    }

    return fail("no such command");
}
