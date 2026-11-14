/*
 * Commands that work with files and folders:
 * ls cp mv rm mkdir rmdir touch ln stat du df basename dirname
 * realpath readlink find chmod chown chgrp file
 */

#include <dirent.h>
#include <fnmatch.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>

#include "coreutils.h"
#include "../libuser.h"

/* ====================================================================
 * names of users and groups (remembered, /etc/passwd is read once each)
 * ==================================================================== */

static const char *user_name(uid_t uid) {

    static uid_t last = (uid_t)-1;
    static char name[64];
    struct bharat_user u;

    if (uid != last) {

        last = uid;

        if (find_user(NULL, uid, &u))
            snprintf(name, sizeof(name), "%s", u.name);
        else
            snprintf(name, sizeof(name), "%u", (unsigned)uid);
    }

    return name;
}

static const char *group_of(gid_t gid) {

    static gid_t last = (gid_t)-1;
    static char name[64];

    if (gid != last) {

        last = gid;
        group_name(gid, name, sizeof(name));

        if (!name[0])
            snprintf(name, sizeof(name), "%u", (unsigned)gid);
    }

    return name;
}

/* "drwxr-xr-x" */
static void mode_string(mode_t m, char out[11]) {

    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' :
             S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';

    const char *rwx = "rwxrwxrwx";

    for (int i = 0; i < 9; i++)
        out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';

    if (m & S_ISUID) out[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID) out[6] = (m & S_IXGRP) ? 's' : 'S';
    if (m & S_ISVTX) out[9] = (m & S_IXOTH) ? 't' : 'T';

    out[10] = 0;
}

/* ====================================================================
 * ls
 * ==================================================================== */

struct entry {
    char name[256];
    struct stat st;
};

static int ls_flags[128];

static int compare(const void *a, const void *b) {

    const struct entry *x = a, *y = b;
    int r;

    if (ls_flags['t'])
        r = (y->st.st_mtime > x->st.st_mtime) - (y->st.st_mtime < x->st.st_mtime);
    else if (ls_flags['S'])
        r = (y->st.st_size > x->st.st_size) - (y->st.st_size < x->st.st_size);
    else
        r = strcasecmp(x->name[0] == '.' ? x->name + 1 : x->name,
                       y->name[0] == '.' ? y->name + 1 : y->name);

    if (r == 0)
        r = strcmp(x->name, y->name);

    return ls_flags['r'] ? -r : r;
}

static int use_color;

static const char *color_for(const struct stat *st) {

    if (!use_color) return "";
    if (S_ISDIR(st->st_mode)) return "\033[1;34m";
    if (S_ISLNK(st->st_mode)) return "\033[1;36m";
    if (st->st_mode & S_IXUSR) return "\033[1;32m";
    return "";
}

static void print_name(const char *name, const struct stat *st) {

    const char *c = color_for(st);

    printf("%s%s%s", c, name, c[0] ? "\033[0m" : "");
}

static void long_line(const char *dir, struct entry *e) {

    char mode[11], size[32], when[32];
    struct tm *tm = localtime(&e->st.st_mtime);
    time_t now = time(NULL);

    mode_string(e->st.st_mode, mode);

    if (ls_flags['h'])
        human(e->st.st_size, size, sizeof(size));
    else
        snprintf(size, sizeof(size), "%lld", (long long)e->st.st_size);

    /* older than 6 months: show the year instead of the time */
    if (now - e->st.st_mtime > 180L * 24 * 3600 || e->st.st_mtime > now + 3600)
        strftime(when, sizeof(when), "%b %e  %Y", tm);
    else
        strftime(when, sizeof(when), "%b %e %H:%M", tm);

    printf("%s %2lu %-8s %-8s %6s %s ", mode, (unsigned long)e->st.st_nlink,
           user_name(e->st.st_uid), group_of(e->st.st_gid), size, when);

    print_name(e->name, &e->st);

    if (S_ISLNK(e->st.st_mode)) {

        char path[PATH_MAX], target[PATH_MAX];
        ssize_t n;

        join(dir, e->name, path, sizeof(path));
        n = readlink(path, target, sizeof(target) - 1);

        if (n >= 0) {

            target[n] = 0;
            printf(" -> %s", target);
        }
    }

    printf("\n");
}

static int terminal_width(void) {

    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;

    return 80;
}

/* names in columns, top to bottom then left to right, like Ubuntu */
static void columns(struct entry *e, int n) {

    if (n == 0)
        return;

    int widest = 0;

    for (int i = 0; i < n; i++) {

        int len = (int)strlen(e[i].name);

        if (len > widest)
            widest = len;
    }

    int col_w = widest + 2;
    int cols = terminal_width() / col_w;

    if (cols < 1)
        cols = 1;

    int rows = (n + cols - 1) / cols;

    for (int r = 0; r < rows; r++) {

        for (int c = 0; c < cols; c++) {

            int i = c * rows + r;

            if (i >= n)
                continue;

            print_name(e[i].name, &e[i].st);

            if ((c + 1) * rows + r < n)
                printf("%*s", col_w - (int)strlen(e[i].name), "");
        }

        printf("\n");
    }
}

static void show(const char *dir, struct entry *e, int n) {

    if (ls_flags['l']) {

        long long blocks = 0;

        for (int i = 0; i < n; i++)
            blocks += (long long)e[i].st.st_blocks;

        if (dir)
            printf("total %lld\n", blocks / 2);

        for (int i = 0; i < n; i++)
            long_line(dir ? dir : ".", &e[i]);
    }
    else if (ls_flags['1'] || !isatty(STDOUT_FILENO)) {

        for (int i = 0; i < n; i++) {

            print_name(e[i].name, &e[i].st);
            printf("\n");
        }
    }
    else {

        columns(e, n);
    }
}

static int list_dir(const char *path, int header) {

    DIR *d = opendir(path);

    if (!d)
        return fail("cannot open '%s': %s", path, strerror(errno));

    int cap = 64, n = 0;
    struct entry *e = malloc(sizeof(struct entry) * cap);
    struct dirent *de;

    while (e && (de = readdir(d)) != NULL) {

        if (de->d_name[0] == '.') {

            if (!ls_flags['a'] && !ls_flags['A'])
                continue;

            if (ls_flags['A'] && (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0))
                continue;
        }

        if (n == cap) {

            cap *= 2;
            e = realloc(e, sizeof(struct entry) * cap);

            if (!e)
                break;
        }

        char full[PATH_MAX];

        snprintf(e[n].name, sizeof(e[n].name), "%s", de->d_name);
        join(path, de->d_name, full, sizeof(full));

        if (lstat(full, &e[n].st) != 0)
            memset(&e[n].st, 0, sizeof(e[n].st));

        n++;
    }

    closedir(d);

    if (!e)
        return fail("out of memory");

    qsort(e, n, sizeof(struct entry), compare);

    if (header)
        printf("%s:\n", path);

    show(path, e, n);

    /* -R: the folders inside, one after the other */
    if (ls_flags['R']) {

        for (int i = 0; i < n; i++) {

            if (S_ISDIR(e[i].st.st_mode) && strcmp(e[i].name, ".") != 0 &&
                strcmp(e[i].name, "..") != 0) {

                char sub[PATH_MAX];

                join(path, e[i].name, sub, sizeof(sub));
                printf("\n");
                list_dir(sub, 1);
            }
        }
    }

    free(e);

    return 0;
}

COMMAND(ls) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "laAh1RdtrS", "", flags, values);

    memcpy(ls_flags, flags, sizeof(flags));
    use_color = isatty(STDOUT_FILENO);

    char *here[] = {"."};
    char **paths = i < argc ? &argv[i] : here;
    int count = i < argc ? argc - i : 1;
    int failed = 0;

    /* files first (all together), then each folder */
    struct entry *files = calloc(count, sizeof(struct entry));
    int nfiles = 0;

    for (int k = 0; k < count; k++) {

        struct stat st;

        if (lstat(paths[k], &st) != 0) {

            failed = fail("cannot access '%s': %s", paths[k], strerror(errno));
            continue;
        }

        /* a link to a folder is shown as the folder (unless -l) */
        if (S_ISLNK(st.st_mode) && !flags['l'])
            stat(paths[k], &st);

        if (!S_ISDIR(st.st_mode) || flags['d']) {

            snprintf(files[nfiles].name, sizeof(files[nfiles].name), "%s", paths[k]);
            files[nfiles++].st = st;
        }
    }

    qsort(files, nfiles, sizeof(struct entry), compare);
    show(NULL, files, nfiles);

    int printed = nfiles;

    for (int k = 0; k < count && !flags['d']; k++) {

        struct stat st;

        if (stat(paths[k], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (printed++)
            printf("\n");

        failed |= list_dir(paths[k], count > 1 || flags['R']);
    }

    free(files);

    return failed;
}

/* ====================================================================
 * cp and mv
 * ==================================================================== */

static int cp_flags[128];

static int copy_path(const char *from, const char *to);

static int copy_file(const char *from, const char *to, const struct stat *st) {

    int in = open(from, O_RDONLY | O_CLOEXEC);

    if (in < 0)
        return fail("cannot open '%s': %s", from, strerror(errno));

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 07777);

    if (out < 0) {

        close(in);
        return fail("cannot create '%s': %s", to, strerror(errno));
    }

    int r = copy_fd(in, out);

    if (cp_flags['p']) {

        struct timespec times[2] = {st->st_atim, st->st_mtim};

        futimens(out, times);
        fchmod(out, st->st_mode & 07777);

        if (fchown(out, st->st_uid, st->st_gid) != 0) {
            /* only root may give files away: keep our own */
        }
    }

    close(in);

    if (close(out) != 0 || r != 0)
        return fail("error while copying '%s'", from);

    return 0;
}

static int copy_dir(const char *from, const char *to, const struct stat *st) {

    if (mkdir(to, (st->st_mode & 07777) | 0700) != 0 && errno != EEXIST)
        return fail("cannot create folder '%s': %s", to, strerror(errno));

    DIR *d = opendir(from);

    if (!d)
        return fail("cannot open '%s': %s", from, strerror(errno));

    struct dirent *e;
    int failed = 0;

    while ((e = readdir(d)) != NULL) {

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char a[PATH_MAX], b[PATH_MAX];

        join(from, e->d_name, a, sizeof(a));
        join(to, e->d_name, b, sizeof(b));
        failed |= copy_path(a, b);
    }

    closedir(d);
    chmod(to, st->st_mode & 07777);

    return failed;
}

static int copy_path(const char *from, const char *to) {

    struct stat st;

    if (lstat(from, &st) != 0)
        return fail("cannot stat '%s': %s", from, strerror(errno));

    if (cp_flags['v'])
        printf("'%s' -> '%s'\n", from, to);

    if (S_ISDIR(st.st_mode)) {

        if (!cp_flags['r'] && !cp_flags['R'] && !cp_flags['a'])
            return fail("-r not given; leaving out folder '%s'", from);

        return copy_dir(from, to, &st);
    }

    if (S_ISLNK(st.st_mode)) {

        char target[PATH_MAX];
        ssize_t n = readlink(from, target, sizeof(target) - 1);

        if (n < 0)
            return fail("cannot read link '%s'", from);

        target[n] = 0;
        unlink(to);

        return symlink(target, to) == 0 ? 0 : fail("cannot create link '%s'", to);
    }

    return copy_file(from, to, &st);
}

/* "cp a b" or "cp a b c folder": where does each one go? */
static void destination(const char *src, const char *dst, int into, char *out, size_t size) {

    if (!into) {

        snprintf(out, size, "%s", dst);
        return;
    }

    char copy[PATH_MAX];

    snprintf(copy, sizeof(copy), "%s", src);
    join(dst, basename(copy), out, size);
}

static int is_dir(const char *p) {

    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int same_file(const char *a, const char *b) {

    struct stat x, y;

    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

COMMAND(cp) {

    char *values[128] = {0};
    int i = options(argc, argv, "rRavpfin", "", cp_flags, values);

    if (cp_flags['a'])
        cp_flags['p'] = 1;

    if (argc - i < 2)
        return fail("usage: cp [-r] [-v] [-p] SOURCE... DEST");

    const char *dst = argv[argc - 1];
    int into = is_dir(dst);

    if (argc - i > 2 && !into)
        return fail("target '%s' is not a folder", dst);

    int failed = 0;

    for (int k = i; k < argc - 1; k++) {

        char to[PATH_MAX];

        destination(argv[k], dst, into, to, sizeof(to));

        if (same_file(argv[k], to)) {

            failed = fail("'%s' and '%s' are the same file", argv[k], to);
            continue;
        }

        if (cp_flags['n'] && access(to, F_OK) == 0)
            continue;

        failed |= copy_path(argv[k], to);
    }

    return failed;
}

static int remove_path(const char *path, int recursive, int force, int verbose);

COMMAND(mv) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "fvin", "", flags, values);

    if (argc - i < 2)
        return fail("usage: mv [-v] SOURCE... DEST");

    const char *dst = argv[argc - 1];
    int into = is_dir(dst);

    if (argc - i > 2 && !into)
        return fail("target '%s' is not a folder", dst);

    int failed = 0;

    for (int k = i; k < argc - 1; k++) {

        char to[PATH_MAX];

        destination(argv[k], dst, into, to, sizeof(to));

        if (flags['n'] && access(to, F_OK) == 0)
            continue;

        if (rename(argv[k], to) == 0) {

            if (flags['v'])
                printf("renamed '%s' -> '%s'\n", argv[k], to);

            continue;
        }

        /* another disk: copy, then delete the original */
        if (errno == EXDEV) {

            memset(cp_flags, 0, sizeof(cp_flags));
            cp_flags['r'] = cp_flags['p'] = 1;

            if (copy_path(argv[k], to) == 0 && remove_path(argv[k], 1, 1, 0) == 0) {

                if (flags['v'])
                    printf("moved '%s' -> '%s'\n", argv[k], to);

                continue;
            }
        }

        failed = fail("cannot move '%s' to '%s': %s", argv[k], to, strerror(errno));
    }

    return failed;
}

/* ====================================================================
 * rm, mkdir, rmdir, touch, ln
 * ==================================================================== */

static int remove_path(const char *path, int recursive, int force, int verbose) {

    struct stat st;

    if (lstat(path, &st) != 0)
        return force && errno == ENOENT ? 0 : fail("cannot remove '%s': %s", path, strerror(errno));

    if (S_ISDIR(st.st_mode)) {

        if (!recursive)
            return fail("cannot remove '%s': it is a folder (use rm -r)", path);

        DIR *d = opendir(path);
        struct dirent *e;
        int failed = 0;

        if (!d)
            return fail("cannot open '%s': %s", path, strerror(errno));

        while ((e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            join(path, e->d_name, child, sizeof(child));
            failed |= remove_path(child, recursive, force, verbose);
        }

        closedir(d);

        if (rmdir(path) != 0)
            return fail("cannot remove '%s': %s", path, strerror(errno));

        if (verbose)
            printf("removed folder '%s'\n", path);

        return failed;
    }

    if (unlink(path) != 0)
        return fail("cannot remove '%s': %s", path, strerror(errno));

    if (verbose)
        printf("removed '%s'\n", path);

    return 0;
}

COMMAND(rm) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "rRfvdi", "", flags, values);
    int recursive = flags['r'] || flags['R'];
    int failed = 0;

    if (i >= argc && !flags['f'])
        return fail("usage: rm [-r] [-f] [-v] FILE...");

    for (; i < argc; i++) {

        char real[PATH_MAX];

        /* the safety rail every system needs */
        if (realpath(argv[i], real) && strcmp(real, "/") == 0) {

            failed = fail("refusing to remove '/'");
            continue;
        }

        if (flags['d'] && is_dir(argv[i])) {

            if (rmdir(argv[i]) != 0)
                failed = fail("cannot remove '%s': %s", argv[i], strerror(errno));

            continue;
        }

        failed |= remove_path(argv[i], recursive, flags['f'], flags['v']);
    }

    return failed;
}

static int make_dirs(char *path, mode_t mode, int verbose) {

    /* create every missing parent, from the top down */
    for (char *p = path + 1; *p; p++) {

        if (*p != '/')
            continue;

        *p = 0;

        if (mkdir(path, 0755) == 0 && verbose)
            printf("created folder '%s'\n", path);

        *p = '/';
    }

    if (mkdir(path, mode) == 0) {

        if (verbose)
            printf("created folder '%s'\n", path);

        return 0;
    }

    return errno == EEXIST && is_dir(path) ? 0 : -1;
}

COMMAND(mkdir) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "pvm", "m", flags, values);
    mode_t mode = values['m'] ? (mode_t)strtol(values['m'], NULL, 8) : 0777;
    int failed = 0;

    if (i >= argc)
        return fail("usage: mkdir [-p] [-v] [-m MODE] FOLDER...");

    for (; i < argc; i++) {

        if (flags['p']) {

            char path[PATH_MAX];

            snprintf(path, sizeof(path), "%s", argv[i]);

            if (make_dirs(path, mode, flags['v']) != 0)
                failed = fail("cannot create folder '%s': %s", argv[i], strerror(errno));
        }
        else if (mkdir(argv[i], mode) != 0) {

            failed = fail("cannot create folder '%s': %s", argv[i], strerror(errno));
        }
        else if (flags['v']) {

            printf("created folder '%s'\n", argv[i]);
        }
    }

    return failed;
}

COMMAND(rmdir) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "pv", "", flags, values);
    int failed = 0;

    if (i >= argc)
        return fail("usage: rmdir [-p] FOLDER...");

    for (; i < argc; i++) {

        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s", argv[i]);

        do {

            if (rmdir(path) != 0) {

                failed = fail("cannot remove '%s': %s", path, strerror(errno));
                break;
            }

            if (flags['v'])
                printf("removed folder '%s'\n", path);

            char *slash = strrchr(path, '/');

            if (!flags['p'] || !slash || slash == path)
                break;

            *slash = 0;

        } while (1);
    }

    return failed;
}

COMMAND(touch) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "cam", "", flags, values);
    int failed = 0;

    if (i >= argc)
        return fail("usage: touch [-c] FILE...");

    for (; i < argc; i++) {

        /* no O_TRUNC: touching an existing file keeps its content */
        int fd = open(argv[i], O_WRONLY | O_CLOEXEC | (flags['c'] ? 0 : O_CREAT), 0644);

        if (fd < 0) {

            if (!(flags['c'] && errno == ENOENT))
                failed = fail("cannot touch '%s': %s", argv[i], strerror(errno));

            continue;
        }

        futimens(fd, NULL);
        close(fd);
    }

    return failed;
}

COMMAND(ln) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "sfvn", "", flags, values);

    if (argc - i < 1)
        return fail("usage: ln [-s] [-f] TARGET [LINK_NAME]");

    const char *dst = argc - i >= 2 ? argv[argc - 1] : ".";
    int last = argc - i >= 2 ? argc - 1 : argc;
    int into = is_dir(dst) && !(flags['n'] && argc - i == 2);
    int failed = 0;

    for (int k = i; k < last; k++) {

        char to[PATH_MAX];

        destination(argv[k], dst, into, to, sizeof(to));

        if (flags['f'])
            unlink(to);

        int r = flags['s'] ? symlink(argv[k], to) : link(argv[k], to);

        if (r != 0)
            failed = fail("cannot create link '%s': %s", to, strerror(errno));
        else if (flags['v'])
            printf("'%s' -> '%s'\n", to, argv[k]);
    }

    return failed;
}

/* ====================================================================
 * stat, du, df
 * ==================================================================== */

COMMAND(stat) {

    int failed = 0;

    if (argc < 2)
        return fail("usage: stat FILE...");

    for (int i = 1; i < argc; i++) {

        struct stat st;

        if (lstat(argv[i], &st) != 0) {

            failed = fail("cannot stat '%s': %s", argv[i], strerror(errno));
            continue;
        }

        char mode[11], t[3][64];
        const time_t *times[3] = {&st.st_atime, &st.st_mtime, &st.st_ctime};

        mode_string(st.st_mode, mode);

        for (int k = 0; k < 3; k++)
            strftime(t[k], sizeof(t[k]), "%Y-%m-%d %H:%M:%S %z", localtime(times[k]));

        const char *type = S_ISDIR(st.st_mode) ? "directory" : S_ISLNK(st.st_mode) ? "symbolic link" :
                           S_ISCHR(st.st_mode) ? "character device" : S_ISBLK(st.st_mode) ? "block device" :
                           S_ISFIFO(st.st_mode) ? "fifo" : S_ISSOCK(st.st_mode) ? "socket" :
                           st.st_size == 0 ? "regular empty file" : "regular file";

        printf("  File: %s\n", argv[i]);
        printf("  Size: %-12lld Blocks: %-10lld IO Block: %-6ld %s\n",
               (long long)st.st_size, (long long)st.st_blocks, (long)st.st_blksize, type);
        printf("Device: %lx    Inode: %-10lu Links: %lu\n",
               (unsigned long)st.st_dev, (unsigned long)st.st_ino, (unsigned long)st.st_nlink);
        printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n",
               st.st_mode & 07777, mode, (unsigned)st.st_uid, user_name(st.st_uid),
               (unsigned)st.st_gid, group_of(st.st_gid));
        printf("Access: %s\nModify: %s\nChange: %s\n", t[0], t[1], t[2]);
    }

    return failed;
}

static int du_flags[128];

/* size of a file or a whole folder, in bytes actually used on disk */
static long long du_walk(const char *path, int depth) {

    struct stat st;

    if (lstat(path, &st) != 0) {

        fail("cannot read '%s': %s", path, strerror(errno));
        return 0;
    }

    long long total = (long long)st.st_blocks * 512;

    if (S_ISDIR(st.st_mode)) {

        DIR *d = opendir(path);
        struct dirent *e;

        while (d && (e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            join(path, e->d_name, child, sizeof(child));
            total += du_walk(child, depth + 1);
        }

        if (d)
            closedir(d);
    }

    int show = depth == 0 || (!du_flags['s'] && (S_ISDIR(st.st_mode) || du_flags['a']));

    if (show) {

        char size[32];

        if (du_flags['h'])
            human(total, size, sizeof(size));
        else
            snprintf(size, sizeof(size), "%lld", total / 1024);

        printf("%s\t%s\n", size, path);
    }

    return total;
}

COMMAND(du) {

    char *values[128] = {0};
    int i = options(argc, argv, "shac", "", du_flags, values);
    long long all = 0;

    if (i >= argc)
        all += du_walk(".", 0);

    for (; i < argc; i++)
        all += du_walk(argv[i], 0);

    if (du_flags['c']) {

        char size[32];

        if (du_flags['h'])
            human(all, size, sizeof(size));
        else
            snprintf(size, sizeof(size), "%lld", all / 1024);

        printf("%s\ttotal\n", size);
    }

    return 0;
}

COMMAND(df) {

    int flags[128] = {0};
    char *values[128] = {0};
    options(argc, argv, "hT", "", flags, values);

    FILE *f = fopen("/proc/mounts", "r");

    if (!f)
        return fail("cannot read /proc/mounts");

    printf(flags['T'] ? "%-16s %-8s %10s %10s %10s %5s %s\n" : "%-16s%.0s %10s %10s %10s %5s %s\n",
           "Filesystem", "Type", flags['h'] ? "Size" : "1K-blocks", "Used", "Available", "Use%", "Mounted on");

    char dev[256], dir[256], type[64], rest[256];

    while (fscanf(f, "%255s %255s %63s %255[^\n]", dev, dir, type, rest) == 4) {

        struct statvfs v;

        if (statvfs(dir, &v) != 0 || v.f_blocks == 0)
            continue;

        long long size = (long long)v.f_blocks * v.f_frsize;
        long long avail = (long long)v.f_bavail * v.f_frsize;
        long long used = size - (long long)v.f_bfree * v.f_frsize;
        int pct = used + avail > 0 ? (int)((used * 100 + used + avail - 1) / (used + avail)) : 0;
        char a[32], b[32], c[32];

        if (flags['h']) {

            human(size, a, sizeof(a));
            human(used, b, sizeof(b));
            human(avail, c, sizeof(c));
        }
        else {

            snprintf(a, sizeof(a), "%lld", size / 1024);
            snprintf(b, sizeof(b), "%lld", used / 1024);
            snprintf(c, sizeof(c), "%lld", avail / 1024);
        }

        printf(flags['T'] ? "%-16s %-8s %10s %10s %10s %4d%% %s\n" : "%-16s%.0s %10s %10s %10s %4d%% %s\n",
               dev, type, a, b, c, pct, dir);
    }

    fclose(f);

    return 0;
}

/* ====================================================================
 * path names: basename dirname realpath readlink
 * ==================================================================== */

COMMAND(basename) {

    if (argc < 2)
        return fail("usage: basename PATH [SUFFIX]");

    char copy[PATH_MAX];

    snprintf(copy, sizeof(copy), "%s", argv[1]);

    char *b = basename(copy);
    size_t n = strlen(b);

    if (argc > 2) {

        size_t s = strlen(argv[2]);

        if (s < n && strcmp(b + n - s, argv[2]) == 0)
            b[n - s] = 0;
    }

    printf("%s\n", b);

    return 0;
}

COMMAND(dirname) {

    if (argc < 2)
        return fail("usage: dirname PATH");

    for (int i = 1; i < argc; i++) {

        char copy[PATH_MAX];

        snprintf(copy, sizeof(copy), "%s", argv[i]);
        printf("%s\n", dirname(copy));
    }

    return 0;
}

COMMAND(realpath) {

    int failed = 0;

    for (int i = 1; i < argc; i++) {

        char out[PATH_MAX];

        if (realpath(argv[i], out))
            printf("%s\n", out);
        else
            failed = fail("%s: %s", argv[i], strerror(errno));
    }

    return argc < 2 ? fail("usage: realpath PATH...") : failed;
}

COMMAND(readlink) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "fen", "", flags, values);
    int failed = 0;

    for (; i < argc; i++) {

        char out[PATH_MAX];

        if (flags['f'] || flags['e']) {

            if (realpath(argv[i], out))
                printf("%s\n", out);
            else
                failed = 1;

            continue;
        }

        ssize_t n = readlink(argv[i], out, sizeof(out) - 1);

        if (n < 0) {

            failed = 1;
            continue;
        }

        out[n] = 0;
        printf("%s%s", out, flags['n'] ? "" : "\n");
    }

    return failed;
}

/* ====================================================================
 * find PATH... [-name P] [-iname P] [-type f|d|l] [-maxdepth N]
 *             [-mindepth N] [-empty] [-size +N[kMG]] [-newer F]
 * ==================================================================== */

struct find_rules {
    const char *name;
    int ignore_case;
    char type;
    int maxdepth, mindepth;
    int empty;
    long long size;           /* bytes; 0 = no test */
    int size_cmp;             /* -1 smaller, 0 exactly, 1 bigger */
    time_t newer;
};

static int matches(const char *path, const struct stat *st, int depth, struct find_rules *r) {

    if (depth < r->mindepth)
        return 0;

    if (r->name) {

        char copy[PATH_MAX];

        snprintf(copy, sizeof(copy), "%s", path);

        if (fnmatch(r->name, basename(copy), r->ignore_case ? FNM_CASEFOLD : 0) != 0)
            return 0;
    }

    if (r->type) {

        char t = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : S_ISREG(st->st_mode) ? 'f' :
                 S_ISCHR(st->st_mode) ? 'c' : S_ISBLK(st->st_mode) ? 'b' : S_ISFIFO(st->st_mode) ? 'p' : 's';

        if (t != r->type)
            return 0;
    }

    if (r->empty) {

        if (S_ISREG(st->st_mode) && st->st_size != 0)
            return 0;

        if (S_ISDIR(st->st_mode)) {

            DIR *d = opendir(path);
            struct dirent *e;
            int n = 0;

            while (d && (e = readdir(d)) != NULL)
                n++;

            if (d)
                closedir(d);

            if (n > 2)
                return 0;
        }
    }

    if (r->size) {

        long long s = st->st_size;

        if ((r->size_cmp > 0 && s <= r->size) || (r->size_cmp < 0 && s >= r->size) ||
            (r->size_cmp == 0 && s != r->size))
            return 0;
    }

    if (r->newer && st->st_mtime <= r->newer)
        return 0;

    return 1;
}

static void find_walk(const char *path, int depth, struct find_rules *r) {

    struct stat st;

    if (lstat(path, &st) != 0) {

        fail("'%s': %s", path, strerror(errno));
        return;
    }

    if (matches(path, &st, depth, r))
        printf("%s\n", path);

    if (!S_ISDIR(st.st_mode) || (r->maxdepth >= 0 && depth >= r->maxdepth))
        return;

    DIR *d = opendir(path);
    struct dirent *e;

    if (!d) {

        fail("'%s': %s", path, strerror(errno));
        return;
    }

    while ((e = readdir(d)) != NULL) {

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];

        join(path, e->d_name, child, sizeof(child));
        find_walk(child, depth + 1, r);
    }

    closedir(d);
}

COMMAND(find) {

    struct find_rules r = {.maxdepth = -1};
    int first_test = 1;

    while (first_test < argc && argv[first_test][0] != '-')
        first_test++;

    for (int i = first_test; i < argc; i++) {

        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "-print") == 0)
            continue;

        if (strcmp(a, "-empty") == 0) {

            r.empty = 1;
            continue;
        }

        if (!v)
            return fail("%s needs a value", a);

        i++;

        if (strcmp(a, "-name") == 0) r.name = v;
        else if (strcmp(a, "-iname") == 0) r.name = v, r.ignore_case = 1;
        else if (strcmp(a, "-type") == 0) r.type = v[0];
        else if (strcmp(a, "-maxdepth") == 0) r.maxdepth = atoi(v);
        else if (strcmp(a, "-mindepth") == 0) r.mindepth = atoi(v);
        else if (strcmp(a, "-newer") == 0) {

            struct stat st;

            if (stat(v, &st) != 0)
                return fail("cannot stat '%s'", v);

            r.newer = st.st_mtime;
        }
        else if (strcmp(a, "-size") == 0) {

            const char *p = v;

            r.size_cmp = *p == '+' ? 1 : *p == '-' ? -1 : 0;

            if (*p == '+' || *p == '-')
                p++;

            char *end;

            r.size = strtoll(p, &end, 10);

            if (*end == 'k') r.size *= 1024;
            else if (*end == 'M') r.size *= 1024 * 1024;
            else if (*end == 'G') r.size *= 1024LL * 1024 * 1024;
            else if (*end == 'c' || *end == 0) { /* bytes */ }
            else r.size *= 512;

            if (r.size == 0 && r.size_cmp >= 0)
                r.size_cmp = 2;     /* "+0": anything not empty, handled below */
        }
        else return fail("unknown test %s (try -name, -type, -maxdepth, -size, -empty, -newer)", a);
    }

    if (r.size_cmp == 2) {

        r.size_cmp = 1;
        r.size = 0;
        r.empty = 0;
    }

    if (first_test == 1)
        find_walk(".", 0, &r);

    for (int i = 1; i < first_test; i++)
        find_walk(argv[i], 0, &r);

    return 0;
}

/* ====================================================================
 * chmod (755 or u+x,g-w,o=r), chown, chgrp
 * ==================================================================== */

/* apply "u+x,go-w" to an old mode; returns -1 if the text is wrong */
static int symbolic_mode(const char *text, mode_t old, int is_dir) {

    mode_t m = old & 07777;
    const char *p = text;

    while (*p) {

        mode_t who = 0;

        for (; *p && strchr("ugoa", *p); p++) {

            if (*p == 'u') who |= 04700;
            if (*p == 'g') who |= 02070;
            if (*p == 'o') who |= 01007;
            if (*p == 'a') who |= 07777;
        }

        if (!who)
            who = 07777 & ~(mode_t)022;     /* like "a", minus the umask */

        if (!*p || !strchr("+-=", *p))
            return -1;

        while (*p && strchr("+-=", *p)) {

            char op = *p++;
            mode_t bits = 0;

            for (; *p && strchr("rwxXst", *p); p++) {

                if (*p == 'r') bits |= 0444;
                if (*p == 'w') bits |= 0222;
                if (*p == 'x') bits |= 0111;
                if (*p == 'X' && (is_dir || (old & 0111))) bits |= 0111;
                if (*p == 's') bits |= 06000;
                if (*p == 't') bits |= 01000;
            }

            bits &= who;

            if (op == '+') m |= bits;
            else if (op == '-') m &= ~bits;
            else m = (m & ~(who & 07777)) | bits;
        }

        if (*p == ',')
            p++;
        else if (*p)
            return -1;
    }

    return (int)m;
}

static int chmod_one(const char *path, const char *spec, int recursive, int verbose) {

    struct stat st;

    if (stat(path, &st) != 0)
        return fail("cannot access '%s': %s", path, strerror(errno));

    int mode;

    if (spec[0] >= '0' && spec[0] <= '7') {

        char *end;
        long v = strtol(spec, &end, 8);

        mode = *end || v > 07777 ? -1 : (int)v;
    }
    else {

        mode = symbolic_mode(spec, st.st_mode, S_ISDIR(st.st_mode));
    }

    if (mode < 0) {

        fail("invalid mode '%s' (examples: 755, u+x, go-w)", spec);
        exit(1);
    }

    int failed = 0;

    if (chmod(path, (mode_t)mode) != 0)
        failed = fail("changing '%s': %s", path, strerror(errno));
    else if (verbose)
        printf("mode of '%s' is now %04o\n", path, mode);

    if (recursive && S_ISDIR(st.st_mode)) {

        DIR *d = opendir(path);
        struct dirent *e;

        while (d && (e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            join(path, e->d_name, child, sizeof(child));
            failed |= chmod_one(child, spec, 1, verbose);
        }

        if (d)
            closedir(d);
    }

    return failed;
}

COMMAND(chmod) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "Rv", "", flags, values);

    /* "chmod -x file" is a mode, not an option: options() would refuse it */
    if (argc - i < 2)
        return fail("usage: chmod [-R] MODE FILE...   (MODE: 755, u+x, go-w, a=r)");

    int failed = 0;

    for (int k = i + 1; k < argc; k++)
        failed |= chmod_one(argv[k], argv[i], flags['R'], flags['v']);

    return failed;
}

static int chown_walk(const char *path, uid_t uid, gid_t gid, int recursive) {

    int failed = 0;

    if (lchown(path, uid, gid) != 0)
        failed = fail("changing owner of '%s': %s", path, strerror(errno));

    struct stat st;

    if (recursive && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {

        DIR *d = opendir(path);
        struct dirent *e;

        while (d && (e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];

            join(path, e->d_name, child, sizeof(child));
            failed |= chown_walk(child, uid, gid, 1);
        }

        if (d)
            closedir(d);
    }

    return failed;
}

/* "bicky", "bicky:users", ":users" */
static int parse_owner(const char *spec, uid_t *uid, gid_t *gid) {

    char copy[128];

    snprintf(copy, sizeof(copy), "%s", spec);

    char *colon = strchr(copy, ':');

    if (colon)
        *colon = 0;

    *uid = (uid_t)-1;
    *gid = (gid_t)-1;

    if (copy[0]) {

        struct bharat_user u;

        if (!find_user(copy, 0, &u))
            return fail("no such user: %s", copy);

        *uid = u.uid;

        /* "bicky:" means: bicky's own group */
        if (colon && !colon[1])
            *gid = u.gid;
    }

    if (colon && colon[1] && !find_group(colon + 1, gid))
        return fail("no such group: %s", colon + 1);

    return 0;
}

COMMAND(chown) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "Rvh", "", flags, values);

    if (argc - i < 2)
        return fail("usage: chown [-R] USER[:GROUP] FILE...");

    uid_t uid;
    gid_t gid;

    if (parse_owner(argv[i], &uid, &gid) != 0)
        return 1;

    int failed = 0;

    for (int k = i + 1; k < argc; k++)
        failed |= chown_walk(argv[k], uid, gid, flags['R']);

    return failed;
}

COMMAND(chgrp) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "Rv", "", flags, values);
    gid_t gid;

    if (argc - i < 2)
        return fail("usage: chgrp [-R] GROUP FILE...");

    if (!find_group(argv[i], &gid))
        return fail("no such group: %s", argv[i]);

    int failed = 0;

    for (int k = i + 1; k < argc; k++)
        failed |= chown_walk(argv[k], (uid_t)-1, gid, flags['R']);

    return failed;
}

/* ====================================================================
 * file: what kind of file is this?
 * ==================================================================== */

COMMAND(file) {

    if (argc < 2)
        return fail("usage: file FILE...");

    for (int i = 1; i < argc; i++) {

        struct stat st;
        const char *kind = "data";

        printf("%s: ", argv[i]);

        if (lstat(argv[i], &st) != 0) {

            printf("cannot open (%s)\n", strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) { printf("directory\n"); continue; }
        if (S_ISLNK(st.st_mode)) {

            char t[PATH_MAX];
            ssize_t n = readlink(argv[i], t, sizeof(t) - 1);

            t[n > 0 ? n : 0] = 0;
            printf("symbolic link to %s\n", t);
            continue;
        }

        if (S_ISCHR(st.st_mode)) { printf("character special\n"); continue; }
        if (S_ISBLK(st.st_mode)) { printf("block special\n"); continue; }
        if (st.st_size == 0) { printf("empty\n"); continue; }

        unsigned char b[512];
        int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, b, sizeof(b)) : -1;

        if (fd >= 0)
            close(fd);

        if (n < 0) {

            printf("cannot read (%s)\n", strerror(errno));
            continue;
        }

        if (n >= 4 && memcmp(b, "\177ELF", 4) == 0) kind = "ELF executable (a program)";
        else if (n >= 8 && memcmp(b, "\211PNG", 4) == 0) kind = "PNG image";
        else if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) kind = "JPEG image";
        else if (n >= 4 && memcmp(b, "GIF8", 4) == 0) kind = "GIF image";
        else if (n >= 4 && memcmp(b, "%PDF", 4) == 0) kind = "PDF document";
        else if (n >= 2 && b[0] == 0x1F && b[1] == 0x8B) kind = "gzip compressed data";
        else if (n >= 4 && memcmp(b, "PK\003\004", 4) == 0) kind = "Zip archive";
        else if (n >= 2 && b[0] == '#' && b[1] == '!') kind = "script, text executable";
        else {

            int text = 1, utf8 = 0;

            for (ssize_t k = 0; k < n; k++) {

                if (b[k] == 0 || (b[k] < 32 && !strchr("\n\r\t\f\b\033", b[k])))
                    text = 0;

                if (b[k] >= 128)
                    utf8 = 1;
            }

            if (text)
                kind = utf8 ? "UTF-8 Unicode text" : "ASCII text";
        }

        printf("%s\n", kind);
    }

    return 0;
}
