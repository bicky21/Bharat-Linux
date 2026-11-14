/*
 * Commands that work with text:
 * cat head tail wc grep sort uniq cut tr tee rev tac nl seq yes true
 * false echo printf cmp diff more less xargs test [
 */

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "coreutils.h"

/* ====================================================================
 * reading whole files into lines
 * ==================================================================== */

struct lines {
    char **line;
    size_t count, cap;
};

static void add_line(struct lines *l, const char *s, size_t len) {

    if (l->count == l->cap) {

        l->cap = l->cap ? l->cap * 2 : 256;
        l->line = realloc(l->line, sizeof(char *) * l->cap);

        if (!l->line) {

            fail("out of memory");
            exit(1);
        }
    }

    l->line[l->count] = malloc(len + 1);
    memcpy(l->line[l->count], s, len);
    l->line[l->count][len] = 0;
    l->count++;
}

static FILE *open_input(const char *name) {

    if (strcmp(name, "-") == 0)
        return stdin;

    FILE *f = fopen(name, "r");

    if (!f)
        fail("%s: %s", name, strerror(errno));

    return f;
}

static void close_input(FILE *f) {

    if (f && f != stdin)
        fclose(f);
}

static int read_lines(const char *name, struct lines *l) {

    FILE *f = open_input(name);

    if (!f)
        return 1;

    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&buf, &cap, f)) > 0) {

        if (buf[n - 1] == '\n')
            n--;

        add_line(l, buf, (size_t)n);
    }

    free(buf);
    close_input(f);

    return 0;
}

/* run fn on each file given, or on standard input when there is none */
#define EACH_INPUT(first, argc, argv, ...)                      \
    do {                                                        \
        char *dash[] = {"-"};                                   \
        char **files_ = (first) < (argc) ? &(argv)[first] : dash; \
        int nfiles_ = (first) < (argc) ? (argc) - (first) : 1;  \
        for (int fi_ = 0; fi_ < nfiles_; fi_++) {               \
            const char *name = files_[fi_];                     \
            (void)name;                                         \
            __VA_ARGS__                                         \
        }                                                       \
    } while (0)

/* ====================================================================
 * cat, tac, rev, nl
 * ==================================================================== */

COMMAND(cat) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "nbsAE", "", flags, values);
    int failed = 0;
    long number = 1;

    EACH_INPUT(i, argc, argv, {

        FILE *f = open_input(name);

        if (!f) {

            failed = 1;
            continue;
        }

        /* plain cat: copy the bytes as fast as possible */
        if (!flags['n'] && !flags['b'] && !flags['s'] && !flags['E'] && !flags['A']) {

            fflush(stdout);

            if (copy_fd(fileno(f), STDOUT_FILENO) != 0)
                failed = fail("%s: read error", name);

            close_input(f);
            continue;
        }

        char *buf = NULL;
        size_t cap = 0;
        ssize_t n;
        int blank_before = 0;

        while ((n = getline(&buf, &cap, f)) > 0) {

            int blank = n == 1 && buf[0] == '\n';

            if (flags['s'] && blank && blank_before)
                continue;

            blank_before = blank;

            if (flags['b'] ? !blank : flags['n'])
                printf("%6ld\t", number++);

            if (flags['E'] || flags['A']) {

                if (buf[n - 1] == '\n')
                    buf[--n] = 0;

                fwrite(buf, 1, (size_t)n, stdout);
                printf("$\n");
            }
            else {

                fwrite(buf, 1, (size_t)n, stdout);
            }
        }

        free(buf);
        close_input(f);
    });

    return failed;
}

COMMAND(tac) {

    int failed = 0;

    EACH_INPUT(1, argc, argv, {

        struct lines l = {0};

        failed |= read_lines(name, &l);

        for (size_t k = l.count; k > 0; k--)
            printf("%s\n", l.line[k - 1]);
    });

    return failed;
}

COMMAND(rev) {

    int failed = 0;

    EACH_INPUT(1, argc, argv, {

        struct lines l = {0};

        failed |= read_lines(name, &l);

        for (size_t k = 0; k < l.count; k++) {

            size_t n = strlen(l.line[k]);

            for (size_t c = n; c > 0; c--)
                putchar(l.line[k][c - 1]);

            putchar('\n');
        }
    });

    return failed;
}

COMMAND(nl) {

    int failed = 0;
    long number = 1;

    EACH_INPUT(1, argc, argv, {

        struct lines l = {0};

        failed |= read_lines(name, &l);

        for (size_t k = 0; k < l.count; k++) {

            if (l.line[k][0])
                printf("%6ld\t%s\n", number++, l.line[k]);
            else
                printf("\n");
        }
    });

    return failed;
}

/* ====================================================================
 * head, tail
 * ==================================================================== */

/* "head -5" is the same as "head -n 5" */
static void old_style_count(int *argc, char **argv) {

    if (*argc > 1 && argv[1][0] == '-' && isdigit((unsigned char)argv[1][1])) {

        static char opt[] = "-n";
        static char *rebuilt[256];
        int n = 0;

        rebuilt[n++] = argv[0];
        rebuilt[n++] = opt;
        rebuilt[n++] = argv[1] + 1;

        for (int i = 2; i < *argc && n < 255; i++)
            rebuilt[n++] = argv[i];

        memcpy(argv, rebuilt, sizeof(char *) * n);
        *argc = n;
    }
}

COMMAND(head) {

    int flags[128] = {0};
    char *values[128] = {0};

    old_style_count(&argc, argv);

    int i = options(argc, argv, "ncqv", "nc", flags, values);
    long count = values['n'] ? atol(values['n']) : 10;
    long bytes = values['c'] ? atol(values['c']) : -1;
    int many = argc - i > 1;
    int failed = 0;

    EACH_INPUT(i, argc, argv, {

        FILE *f = open_input(name);

        if (!f) {

            failed = 1;
            continue;
        }

        if (many && !flags['q'])
            printf("%s==> %s <==\n", fi_ ? "\n" : "", name);

        int c;

        if (bytes >= 0) {

            for (long k = 0; k < bytes && (c = getc(f)) != EOF; k++)
                putchar(c);
        }
        else {

            for (long lines = 0; lines < count && (c = getc(f)) != EOF;) {

                putchar(c);

                if (c == '\n')
                    lines++;
            }
        }

        close_input(f);
    });

    return failed;
}

COMMAND(tail) {

    int flags[128] = {0};
    char *values[128] = {0};

    old_style_count(&argc, argv);

    int i = options(argc, argv, "nfqv", "n", flags, values);
    const char *nval = values['n'] ? values['n'] : "10";
    int from_start = nval[0] == '+';
    long count = atol(from_start ? nval + 1 : nval);
    int many = argc - i > 1;
    int failed = 0;

    EACH_INPUT(i, argc, argv, {

        struct lines l = {0};

        if (read_lines(name, &l) != 0) {

            failed = 1;
            continue;
        }

        if (many && !flags['q'])
            printf("%s==> %s <==\n", fi_ ? "\n" : "", name);

        size_t start = from_start ? (size_t)(count > 0 ? count - 1 : 0)
                                  : (l.count > (size_t)count ? l.count - (size_t)count : 0);

        for (size_t k = start; k < l.count; k++)
            printf("%s\n", l.line[k]);
    });

    /* -f: keep watching the (last) file for new lines */
    if (flags['f'] && i < argc) {

        FILE *f = fopen(argv[argc - 1], "r");

        if (!f)
            return fail("%s: %s", argv[argc - 1], strerror(errno));

        fseek(f, 0, SEEK_END);
        fflush(stdout);

        char buf[4096];

        while (1) {

            size_t n = fread(buf, 1, sizeof(buf), f);

            if (n > 0) {

                fwrite(buf, 1, n, stdout);
                fflush(stdout);
            }
            else {

                clearerr(f);
                usleep(300 * 1000);
            }
        }
    }

    return failed;
}

/* ====================================================================
 * wc
 * ==================================================================== */

COMMAND(wc) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "lwcm", "", flags, values);
    int all = !flags['l'] && !flags['w'] && !flags['c'] && !flags['m'];
    long long tl = 0, tw = 0, tc = 0;
    int failed = 0, files = 0;

    EACH_INPUT(i, argc, argv, {

        FILE *f = open_input(name);

        if (!f) {

            failed = 1;
            continue;
        }

        long long lines = 0, words = 0, chars = 0;
        int c, in_word = 0;

        while ((c = getc(f)) != EOF) {

            chars++;

            if (c == '\n')
                lines++;

            if (isspace(c))
                in_word = 0;
            else if (!in_word)
                in_word = 1, words++;
        }

        close_input(f);

        long long counts[3] = {lines, words, chars};
        int show[3] = {all || flags['l'], all || flags['w'], all || flags['c'] || flags['m']};
        int from_pipe = strcmp(name, "-") == 0;
        int shown = show[0] + show[1] + show[2];
        int first = 1;

        for (int k = 0; k < 3; k++) {

            if (!show[k])
                continue;

            /* one number from a pipe: just the number, like Ubuntu */
            printf(from_pipe && shown == 1 ? "%s%lld" : "%s%7lld", first ? "" : " ", counts[k]);
            first = 0;
        }

        printf(from_pipe ? "\n" : " %s\n", name);

        tl += lines, tw += words, tc += chars;
        files++;
    });

    if (files > 1) {

        if (all || flags['l']) printf("%7lld ", tl);
        if (all || flags['w']) printf("%7lld ", tw);
        if (all || flags['c'] || flags['m']) printf("%7lld ", tc);

        printf("total\n");
    }

    return failed;
}

/* ====================================================================
 * grep
 * ==================================================================== */

static int g[128];
static regex_t pattern;
static const char *fixed;
static int found_any;
static int color;

/* does this line match? start and end: where (for colouring) */
static int line_matches(const char *line, regoff_t *start, regoff_t *end) {

    int hit;

    if (fixed) {

        const char *p = g['i'] ? strcasestr(line, fixed) : strstr(line, fixed);

        hit = p != NULL;

        if (hit && start) {

            *start = p - line;
            *end = *start + (regoff_t)strlen(fixed);
        }
    }
    else {

        regmatch_t m;

        hit = regexec(&pattern, line, 1, &m, 0) == 0;

        if (hit && start) {

            *start = m.rm_so;
            *end = m.rm_eo;
        }
    }

    return g['v'] ? !hit : hit;
}

static int grep_stream(FILE *f, const char *name, int show_name) {

    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;
    long number = 0, count = 0;

    while ((n = getline(&buf, &cap, f)) > 0) {

        number++;

        if (buf[n - 1] == '\n')
            buf[n - 1] = 0;

        regoff_t s = 0, e = 0;

        if (!line_matches(buf, &s, &e))
            continue;

        count++;
        found_any = 1;

        if (g['q'])
            break;

        if (g['l']) {

            printf("%s\n", name);
            break;
        }

        if (g['c'])
            continue;

        if (show_name)
            printf(color ? "\033[35m%s\033[36m:\033[0m" : "%s:", name);

        if (g['n'])
            printf(color ? "\033[32m%ld\033[36m:\033[0m" : "%ld:", number);

        if (g['o'] && !g['v']) {

            printf("%.*s\n", (int)(e - s), buf + s);
        }
        else if (color && !g['v'] && e > s) {

            printf("%.*s\033[1;31m%.*s\033[0m%s\n", (int)s, buf, (int)(e - s), buf + s, buf + e);
        }
        else {

            printf("%s\n", buf);
        }
    }

    if (g['c'])
        printf(show_name ? "%s:%ld\n" : "%.0s%ld\n", name, count);

    free(buf);

    return 0;
}

static void grep_path(const char *path, int show_name) {

    struct stat st;

    if (stat(path, &st) != 0) {

        if (!g['s'])
            fail("%s: %s", path, strerror(errno));

        return;
    }

    if (S_ISDIR(st.st_mode)) {

        if (!g['r'] && !g['R']) {

            fail("%s: is a folder (use grep -r)", path);
            return;
        }

        DIR *d = opendir(path);
        struct dirent *e;

        while (d && (e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            join(path, e->d_name, child, sizeof(child));
            grep_path(child, 1);
        }

        if (d)
            closedir(d);

        return;
    }

    FILE *f = fopen(path, "r");

    if (!f) {

        if (!g['s'])
            fail("%s: %s", path, strerror(errno));

        return;
    }

    grep_stream(f, path, show_name);
    fclose(f);
}

COMMAND(grep) {

    char *values[128] = {0};
    int i = options(argc, argv, "ivnclrRqEFwxhHos", "e", g, values);
    const char *pat = values['e'];

    if (!pat) {

        if (i >= argc)
            return fail("usage: grep [-i -v -n -c -l -r -w -E -F] PATTERN [FILE...]"), 2;

        pat = argv[i++];
    }

    color = isatty(STDOUT_FILENO);

    char built[1024];

    if (g['w'] || g['x']) {

        /* whole words / whole lines: wrap the pattern */
        if (g['F']) {

            /* turn the fixed text into a safe pattern */
            char esc[512];
            size_t k = 0;

            for (const char *p = pat; *p && k < sizeof(esc) - 3; p++) {

                if (strchr("\\.[]()*+?{}|^$", *p))
                    esc[k++] = '\\';

                esc[k++] = *p;
            }

            esc[k] = 0;
            if (g['x'])
                snprintf(built, sizeof(built), "^(%s)$", esc);
            else
                snprintf(built, sizeof(built), "(^|[^[:alnum:]_])(%s)([^[:alnum:]_]|$)", esc);
            g['E'] = 1;
            g['F'] = 0;
        }
        else {

            snprintf(built, sizeof(built), g['E'] ? (g['x'] ? "^(%s)$" : "\\b(%s)\\b")
                                                  : (g['x'] ? "^\\(%s\\)$" : "\\b\\(%s\\)\\b"), pat);
        }

        pat = built;
    }

    if (g['F']) {

        fixed = pat;
    }
    else {

        int cflags = (g['E'] ? REG_EXTENDED : 0) | (g['i'] ? REG_ICASE : 0);
        int r = regcomp(&pattern, pat, cflags);

        if (r != 0) {

            char msg[128];

            regerror(r, &pattern, msg, sizeof(msg));
            return fail("bad pattern: %s", msg), 2;
        }
    }

    int show_name = g['H'] || (!g['h'] && (argc - i > 1 || g['r'] || g['R']));

    if (i >= argc) {

        if (g['r'] || g['R'])
            grep_path(".", 1);
        else
            grep_stream(stdin, "(standard input)", 0);
    }

    for (; i < argc; i++) {

        if (strcmp(argv[i], "-") == 0)
            grep_stream(stdin, "(standard input)", show_name);
        else
            grep_path(argv[i], show_name);
    }

    return found_any ? 0 : 1;
}

/* ====================================================================
 * sort, uniq
 * ==================================================================== */

static int s[128];
static int key_field;
static char key_sep;

/* the text to compare: the whole line, or field number key_field */
static const char *sort_key(const char *line) {

    if (key_field <= 1)
        return line;

    const char *p = line;

    for (int f = 1; f < key_field && *p; f++) {

        if (key_sep) {

            const char *next = strchr(p, key_sep);

            p = next ? next + 1 : p + strlen(p);
        }
        else {

            while (*p && isspace((unsigned char)*p)) p++;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
    }

    return p;
}

static int key_compare(const void *a, const void *b);

/* equal keys: compare the whole lines, so the order is always the same */
static int sort_compare(const void *a, const void *b) {

    int r = key_compare(a, b);

    if (r == 0 && !s['u']) {

        r = strcmp(*(char *const *)a, *(char *const *)b);

        if (s['r'])
            r = -r;
    }

    return r;
}

static int key_compare(const void *a, const void *b) {

    const char *x = sort_key(*(char *const *)a);
    const char *y = sort_key(*(char *const *)b);
    int r;

    if (s['n'] || s['h']) {

        double dx = strtod(x, NULL), dy = strtod(y, NULL);

        r = (dx > dy) - (dx < dy);
    }
    else {

        r = s['f'] ? strcasecmp(x, y) : strcmp(x, y);
    }

    return s['r'] ? -r : r;
}

COMMAND(sort) {

    char *values[128] = {0};
    int i = options(argc, argv, "rnufhkto", "kto", s, values);
    struct lines l = {0};
    int failed = 0;

    key_field = values['k'] ? atoi(values['k']) : 0;
    key_sep = values['t'] ? values['t'][0] : 0;

    EACH_INPUT(i, argc, argv, {

        failed |= read_lines(name, &l);
    });

    qsort(l.line, l.count, sizeof(char *), sort_compare);

    FILE *out = stdout;

    if (values['o'] && !(out = fopen(values['o'], "w")))
        return fail("%s: %s", values['o'], strerror(errno));

    for (size_t k = 0; k < l.count; k++) {

        if (s['u'] && k > 0 && key_compare(&l.line[k], &l.line[k - 1]) == 0)
            continue;

        fprintf(out, "%s\n", l.line[k]);
    }

    if (out != stdout)
        fclose(out);

    return failed;
}

COMMAND(uniq) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "cdui", "", flags, values);
    struct lines l = {0};

    if (read_lines(i < argc ? argv[i] : "-", &l) != 0)
        return 1;

    for (size_t k = 0; k < l.count;) {

        size_t same = 1;

        while (k + same < l.count &&
               (flags['i'] ? strcasecmp(l.line[k], l.line[k + same]) : strcmp(l.line[k], l.line[k + same])) == 0)
            same++;

        int show = flags['d'] ? same > 1 : flags['u'] ? same == 1 : 1;

        if (show) {

            if (flags['c'])
                printf("%7zu %s\n", same, l.line[k]);
            else
                printf("%s\n", l.line[k]);
        }

        k += same;
    }

    return 0;
}

/* ====================================================================
 * cut, tr, tee
 * ==================================================================== */

/* "1,3-5,7-" -> wanted[1], wanted[3..5], wanted[7..] */
static int wanted_from(const char *list, char wanted[1024]) {

    memset(wanted, 0, 1024);

    const char *p = list;

    while (*p) {

        int a = isdigit((unsigned char)*p) ? (int)strtol(p, (char **)&p, 10) : 1;
        int b = a;

        if (*p == '-') {

            p++;
            b = isdigit((unsigned char)*p) ? (int)strtol(p, (char **)&p, 10) : 1023;
        }

        if (a < 1 || b < a || b > 1023)
            return -1;

        for (int k = a; k <= b; k++)
            wanted[k] = 1;

        if (*p == ',')
            p++;
        else if (*p)
            return -1;
    }

    return 0;
}

COMMAND(cut) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "dfcs", "dfc", flags, values);
    char wanted[1024];
    char sep = values['d'] ? values['d'][0] : '\t';
    const char *list = values['f'] ? values['f'] : values['c'];

    if (!list || wanted_from(list, wanted) != 0)
        return fail("usage: cut -f LIST [-d SEP] | cut -c LIST [FILE...]   (LIST like 1,3-5)");

    int failed = 0;

    EACH_INPUT(i, argc, argv, {

        struct lines l = {0};

        failed |= read_lines(name, &l);

        for (size_t k = 0; k < l.count; k++) {

            const char *line = l.line[k];

            if (values['c']) {

                for (int c = 0; line[c]; c++)
                    if (c + 1 < 1024 && wanted[c + 1])
                        putchar(line[c]);

                putchar('\n');
                continue;
            }

            if (!strchr(line, sep)) {

                if (!flags['s'])
                    printf("%s\n", line);

                continue;
            }

            int field = 1, first = 1;
            const char *start = line;

            while (1) {

                const char *end = strchr(start, sep);
                size_t len = end ? (size_t)(end - start) : strlen(start);

                if (field < 1024 && wanted[field]) {

                    if (!first)
                        putchar(sep);

                    fwrite(start, 1, len, stdout);
                    first = 0;
                }

                if (!end)
                    break;

                start = end + 1;
                field++;
            }

            putchar('\n');
        }
    });

    return failed;
}

/* "a-z" -> "abc...z", "\n" -> newline, "[:upper:]" -> A..Z */
static int expand_set(const char *in, char *out, int size) {

    int n = 0;

    for (const char *p = in; *p && n < size - 1; p++) {

        if (strncmp(p, "[:upper:]", 9) == 0) { for (char c = 'A'; c <= 'Z'; c++) out[n++] = c; p += 8; continue; }
        if (strncmp(p, "[:lower:]", 9) == 0) { for (char c = 'a'; c <= 'z'; c++) out[n++] = c; p += 8; continue; }
        if (strncmp(p, "[:digit:]", 9) == 0) { for (char c = '0'; c <= '9'; c++) out[n++] = c; p += 8; continue; }
        if (strncmp(p, "[:space:]", 9) == 0) { const char *sp = " \t\n\r\f\v"; while (*sp) out[n++] = *sp++; p += 8; continue; }

        char c = *p;

        if (c == '\\' && p[1]) {

            p++;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == '\\' ? '\\' : *p;
        }

        if (p[1] == '-' && p[2]) {

            char last = p[2];

            for (int k = (unsigned char)c; k <= (unsigned char)last && n < size - 1; k++)
                out[n++] = (char)k;

            p += 2;
            continue;
        }

        out[n++] = c;
    }

    out[n] = 0;

    return n;
}

COMMAND(tr) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "dsc", "", flags, values);
    char set1[1024], set2[1024];

    if (i >= argc)
        return fail("usage: tr [-d] [-s] SET1 [SET2]   (example: tr a-z A-Z)");

    int n1 = expand_set(argv[i], set1, sizeof(set1));
    int n2 = i + 1 < argc ? expand_set(argv[i + 1], set2, sizeof(set2)) : 0;
    unsigned char map[256];
    char drop[256] = {0}, squeeze[256] = {0};

    for (int k = 0; k < 256; k++)
        map[k] = (unsigned char)k;

    for (int k = 0; k < n1; k++) {

        unsigned char c = (unsigned char)set1[k];

        if (flags['d'])
            drop[c] = 1;
        else if (n2 > 0)
            map[c] = (unsigned char)set2[k < n2 ? k : n2 - 1];

        if (flags['s'] && n2 == 0)
            squeeze[c] = 1;
    }

    for (int k = 0; flags['s'] && k < n2; k++)
        squeeze[(unsigned char)set2[k]] = 1;

    int c, last = -1;

    while ((c = getchar()) != EOF) {

        if (drop[c])
            continue;

        int out = map[c];

        if (squeeze[out] && out == last)
            continue;

        putchar(out);
        last = out;
    }

    return 0;
}

COMMAND(tee) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "a", "", flags, values);
    FILE *out[64];
    int n = 0, failed = 0;

    for (; i < argc && n < 64; i++) {

        out[n] = fopen(argv[i], flags['a'] ? "a" : "w");

        if (out[n])
            n++;
        else
            failed = fail("%s: %s", argv[i], strerror(errno));
    }

    char buf[4096];
    size_t got;

    while ((got = fread(buf, 1, sizeof(buf), stdin)) > 0) {

        fwrite(buf, 1, got, stdout);
        fflush(stdout);

        for (int k = 0; k < n; k++)
            fwrite(buf, 1, got, out[k]);
    }

    for (int k = 0; k < n; k++)
        fclose(out[k]);

    return failed;
}

/* ====================================================================
 * seq, yes, true, false
 * ==================================================================== */

COMMAND(seq) {

    if (argc < 2 || argc > 4)
        return fail("usage: seq [FIRST [STEP]] LAST");

    double first = argc > 2 ? atof(argv[1]) : 1;
    double step = argc > 3 ? atof(argv[2]) : 1;
    double last = atof(argv[argc - 1]);
    int whole = !strchr(argv[1], '.') && (argc < 3 || !strchr(argv[argc - 1], '.')) &&
                (argc < 4 || !strchr(argv[2], '.'));

    if (step == 0)
        return fail("the step must not be 0");

    for (double v = first; step > 0 ? v <= last + 1e-9 : v >= last - 1e-9; v += step) {

        if (whole)
            printf("%.0f\n", v);
        else
            printf("%g\n", v);
    }

    return 0;
}

COMMAND(yes) {

    const char *word = argc > 1 ? argv[1] : "y";

    signal(SIGPIPE, SIG_DFL);

    while (printf("%s\n", word) >= 0) {
        /* until the reader stops reading */
    }

    return 0;
}

COMMAND(true)  { (void)argc; (void)argv; return 0; }
COMMAND(false) { (void)argc; (void)argv; return 1; }

/* ====================================================================
 * echo, printf
 * ==================================================================== */

/* writes text with \n \t \\ ... turned into real characters; returns 1 at \c */
static int print_escaped(const char *p) {

    for (; *p; p++) {

        if (*p != '\\' || !p[1]) {

            putchar(*p);
            continue;
        }

        p++;

        switch (*p) {

            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'a': putchar('\a'); break;
            case 'b': putchar('\b'); break;
            case 'e': putchar(27); break;
            case '\\': putchar('\\'); break;
            case 'c': return 1;
            case '0': {

                int v = 0;

                for (int k = 0; k < 3 && p[1] >= '0' && p[1] <= '7'; k++)
                    v = v * 8 + (*++p - '0');

                putchar(v);
                break;
            }
            default: putchar('\\'); putchar(*p);
        }
    }

    return 0;
}

COMMAND(echo) {

    int newline = 1, escapes = 0, i = 1;

    /* echo has no "--": only -n, -e, -E (and combinations) are options */
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {

        const char *o = argv[i] + 1;

        if (strspn(o, "neE") != strlen(o))
            break;

        for (; *o; o++) {

            if (*o == 'n') newline = 0;
            if (*o == 'e') escapes = 1;
            if (*o == 'E') escapes = 0;
        }
    }

    for (int first = i; i < argc; i++) {

        if (i > first)
            putchar(' ');

        if (escapes ? print_escaped(argv[i]) : (fputs(argv[i], stdout), 0))
            return 0;
    }

    if (newline)
        putchar('\n');

    return 0;
}

COMMAND(printf) {

    if (argc < 2)
        return fail("usage: printf FORMAT [ARGUMENT...]");

    const char *fmt = argv[1];
    int arg = 2;

    /* the format is used again until all arguments are printed */
    do {

        for (const char *p = fmt; *p; p++) {

            if (*p == '\\') {

                char one[3] = {'\\', p[1], 0};

                if (p[1])
                    p++;

                print_escaped(one);
                continue;
            }

            if (*p != '%') {

                putchar(*p);
                continue;
            }

            if (p[1] == '%') {

                putchar('%');
                p++;
                continue;
            }

            /* copy "%-10.2f" into spec */
            char spec[32];
            int k = 0;

            spec[k++] = *p++;

            while (*p && strchr("-+ #0123456789.", *p) && k < 28)
                spec[k++] = *p++;

            char conv = *p;
            const char *value = arg < argc ? argv[arg++] : "";

            if (!conv)
                break;

            if (strchr("di", conv)) {

                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = 'd';
                spec[k] = 0;
                printf(spec, strtoll(value, NULL, 0));
            }
            else if (strchr("uxXo", conv)) {

                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = conv;
                spec[k] = 0;
                printf(spec, (unsigned long long)strtoll(value, NULL, 0));
            }
            else if (strchr("feEgG", conv)) {

                spec[k++] = conv;
                spec[k] = 0;
                printf(spec, strtod(value, NULL));
            }
            else if (conv == 'c') {

                putchar(value[0]);
            }
            else if (conv == 'b') {

                print_escaped(value);
            }
            else {

                spec[k++] = 's';
                spec[k] = 0;
                printf(spec, value);
            }
        }

    } while (arg < argc && arg > 2);

    return 0;
}

/* ====================================================================
 * cmp, diff
 * ==================================================================== */

COMMAND(cmp) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "s", "", flags, values);

    if (argc - i != 2)
        return fail("usage: cmp [-s] FILE1 FILE2"), 2;

    FILE *a = open_input(argv[i]), *b = open_input(argv[i + 1]);

    if (!a || !b)
        return 2;

    long byte = 1, line = 1;
    int x, y;

    while (1) {

        x = getc(a);
        y = getc(b);

        if (x != y) {

            if (x == EOF || y == EOF) {

                if (!flags['s'])
                    fprintf(stderr, "cmp: EOF on %s\n", x == EOF ? argv[i] : argv[i + 1]);
            }
            else if (!flags['s']) {

                printf("%s %s differ: byte %ld, line %ld\n", argv[i], argv[i + 1], byte, line);
            }

            return 1;
        }

        if (x == EOF)
            return 0;

        if (x == '\n')
            line++;

        byte++;
    }
}

/*
 * diff FILE1 FILE2: shows how to turn the first file into the second.
 * Works by finding the longest list of lines both files share (LCS).
 * Output: "-" lines only in FILE1, "+" lines only in FILE2 (-u style).
 */
COMMAND(diff) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "uq", "", flags, values);

    if (argc - i != 2)
        return fail("usage: diff [-q] FILE1 FILE2"), 2;

    struct lines a = {0}, b = {0};

    if (read_lines(argv[i], &a) || read_lines(argv[i + 1], &b))
        return 2;

    size_t n = a.count, m = b.count;

    if (n > 5000 || m > 5000)
        return fail("files are too long for this diff (5000 lines)"), 2;

    /* len[x][y] = shared lines from a[x..] and b[y..] */
    unsigned short *len = calloc((n + 1) * (m + 1), sizeof(unsigned short));

    if (!len)
        return fail("out of memory"), 2;

#define LEN(x, y) len[(size_t)(x) * (m + 1) + (y)]

    for (size_t x = n; x-- > 0;)
        for (size_t y = m; y-- > 0;)
            LEN(x, y) = strcmp(a.line[x], b.line[y]) == 0 ? LEN(x + 1, y + 1) + 1
                       : (LEN(x + 1, y) > LEN(x, y + 1) ? LEN(x + 1, y) : LEN(x, y + 1));

    if (LEN(0, 0) == n && n == m) {

        free(len);
        return 0;
    }

    if (flags['q']) {

        printf("Files %s and %s differ\n", argv[i], argv[i + 1]);
        free(len);
        return 1;
    }

    int tty = isatty(STDOUT_FILENO);

    printf("--- %s\n+++ %s\n", argv[i], argv[i + 1]);

    size_t x = 0, y = 0;

    while (x < n || y < m) {

        if (x < n && y < m && strcmp(a.line[x], b.line[y]) == 0) {

            printf(" %s\n", a.line[x]);
            x++, y++;
        }
        else if (y < m && (x == n || LEN(x, y + 1) > LEN(x + 1, y))) {

            printf(tty ? "\033[32m+%s\033[0m\n" : "+%s\n", b.line[y++]);
        }
        else {

            printf(tty ? "\033[31m-%s\033[0m\n" : "-%s\n", a.line[x++]);
        }
    }

#undef LEN

    free(len);

    return 1;
}

/* ====================================================================
 * more / less: show text one screen at a time
 * ==================================================================== */

static int screen_rows(void) {

    struct winsize ws;
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    int rows = 24;

    if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
        rows = ws.ws_row;

    if (fd >= 0)
        close(fd);

    return rows;
}

/* one key from the keyboard (even when the text comes through a pipe) */
static int read_key(int tty) {

    unsigned char c[4];
    ssize_t n = read(tty, c, sizeof(c));

    if (n <= 0)
        return 'q';

    if (n >= 3 && c[0] == 27 && c[1] == '[') {

        if (c[2] == 'A') return 'k';
        if (c[2] == 'B') return 'j';
        if (c[2] == '5') return 'b';
        if (c[2] == '6') return ' ';
    }

    return c[0];
}

static int pager(int argc, char **argv) {

    struct lines l = {0};
    int failed = 0;

    EACH_INPUT(1, argc, argv, {

        failed |= read_lines(name, &l);
    });

    /* not on a screen (for example piped into another command): just print */
    if (!isatty(STDOUT_FILENO)) {

        for (size_t k = 0; k < l.count; k++)
            printf("%s\n", l.line[k]);

        return failed;
    }

    int tty = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    struct termios old, raw;

    if (tty < 0 || tcgetattr(tty, &old) != 0) {

        for (size_t k = 0; k < l.count; k++)
            printf("%s\n", l.line[k]);

        return failed;
    }

    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSANOW, &raw);

    int page = screen_rows() - 1;
    long top = 0;

    while (1) {

        /* clear the screen and draw one page */
        printf("\033[H\033[2J");

        for (long k = top; k < top + page && k < (long)l.count; k++)
            printf("%s\n", l.line[k]);

        long end = top + page < (long)l.count ? top + page : (long)l.count;

        printf("\033[7m lines %ld-%ld of %zu  (space: next, b: back, arrows, q: quit) \033[0m",
               l.count ? top + 1 : 0, end, l.count);
        fflush(stdout);

        int key = read_key(tty);

        if (key == 'q' || key == 'Q')
            break;
        else if (key == ' ' || key == 'f')
            top += page;
        else if (key == 'b')
            top -= page;
        else if (key == 'j' || key == '\n' || key == '\r')
            top++;
        else if (key == 'k')
            top--;
        else if (key == 'g')
            top = 0;
        else if (key == 'G')
            top = (long)l.count - page;

        if (top > (long)l.count - page)
            top = (long)l.count - page;

        if (top < 0)
            top = 0;
    }

    printf("\033[H\033[2J");
    tcsetattr(tty, TCSANOW, &old);
    close(tty);

    return failed;
}

COMMAND(more) { return pager(argc, argv); }
COMMAND(less) { return pager(argc, argv); }

/* ====================================================================
 * xargs: build a command from the words on standard input
 * ==================================================================== */

COMMAND(xargs) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "n", "n", flags, values);
    int per_run = values['n'] ? atoi(values['n']) : 1000;

    char *cmd[1100];
    int base = 0;

    if (i >= argc)
        cmd[base++] = "echo";

    for (; i < argc && base < 64; i++)
        cmd[base++] = argv[i];

    char word[4096];
    int n = base, failed = 0, more = 1;

    while (more) {

        more = scanf("%4095s", word) == 1;

        if (more)
            cmd[n++] = strdup(word);

        if ((n - base >= per_run || (!more && n > base)) && n > 0) {

            cmd[n] = NULL;

            pid_t pid = fork();

            if (pid == 0) {

                execvp(cmd[0], cmd);
                fail("%s: %s", cmd[0], strerror(errno));
                _exit(127);
            }

            int st;

            waitpid(pid, &st, 0);

            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
                failed = 123;

            n = base;
        }
    }

    return failed;
}

/* ====================================================================
 * test / [ : checks for shell scripts
 *   test -f FILE    test -d DIR    test -e PATH   test -z TEXT
 *   test A = B      test 3 -lt 5   test ! -e FILE
 * ==================================================================== */

static int test_eval(int argc, char **argv) {

    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "!") == 0)
        return !test_eval(argc - 1, argv + 1);

    if (argc == 1)
        return argv[0][0] != 0;

    if (argc == 2) {

        const char *op = argv[0], *v = argv[1];
        struct stat st;
        int exists = stat(v, &st) == 0;

        if (!strcmp(op, "-e")) return exists;
        if (!strcmp(op, "-f")) return exists && S_ISREG(st.st_mode);
        if (!strcmp(op, "-d")) return exists && S_ISDIR(st.st_mode);
        if (!strcmp(op, "-s")) return exists && st.st_size > 0;
        if (!strcmp(op, "-r")) return access(v, R_OK) == 0;
        if (!strcmp(op, "-w")) return access(v, W_OK) == 0;
        if (!strcmp(op, "-x")) return access(v, X_OK) == 0;
        if (!strcmp(op, "-L") || !strcmp(op, "-h")) return lstat(v, &st) == 0 && S_ISLNK(st.st_mode);
        if (!strcmp(op, "-z")) return v[0] == 0;
        if (!strcmp(op, "-n")) return v[0] != 0;
    }

    if (argc == 3) {

        const char *a = argv[0], *op = argv[1], *b = argv[2];
        long long x = atoll(a), y = atoll(b);

        if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(a, b) == 0;
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y;
        if (!strcmp(op, "-ge")) return x >= y;
    }

    fail("cannot understand this test");
    exit(2);
}

COMMAND(test) {

    return test_eval(argc - 1, argv + 1) ? 0 : 1;
}

/* "[ -f x ]": the same as test, with a closing ] */
int cmd_bracket(int argc, char **argv) {

    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0)
        return fail("missing ]"), 2;

    return test_eval(argc - 2, argv + 1) ? 0 : 1;
}
