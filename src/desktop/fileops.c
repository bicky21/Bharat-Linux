/*
 * fileops.c - copying, moving and deleting files and folders for the
 * Files app (copy, cut, paste, Trash, delete forever).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fileops.h"

static void path_join(char *out, size_t size, const char *dir, const char *name) {

    snprintf(out, size, "%.3000s%s%.1000s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

/* copies one file or a whole folder; returns 0 or an errno */
int copy_path(const char *from, const char *to) {

    struct stat st;

    if (lstat(from, &st) != 0)
        return errno;

    if (S_ISLNK(st.st_mode)) {

        char target[PATH_MAX];
        ssize_t n = readlink(from, target, sizeof(target) - 1);

        if (n < 0)
            return errno;

        target[n] = 0;

        return symlink(target, to) == 0 ? 0 : errno;
    }

    if (S_ISDIR(st.st_mode)) {

        if (mkdir(to, st.st_mode & 07777) != 0)
            return errno;

        DIR *d = opendir(from);
        struct dirent *e;
        int err = 0;

        if (!d)
            return errno;

        while (!err && (e = readdir(d)) != NULL) {

            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;

            char a[PATH_MAX], b[PATH_MAX];

            path_join(a, sizeof(a), from, e->d_name);
            path_join(b, sizeof(b), to, e->d_name);
            err = copy_path(a, b);
        }

        closedir(d);

        return err;
    }

    if (!S_ISREG(st.st_mode))
        return 0;      /* devices, sockets: skipped */

    int in = open(from, O_RDONLY | O_CLOEXEC);

    if (in < 0)
        return errno;

    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);

    if (out < 0) {

        int e = errno;

        close(in);
        return e;
    }

    static char buf[1 << 16];
    ssize_t n;
    int err = 0;

    while ((n = read(in, buf, sizeof(buf))) > 0) {

        if (write(out, buf, (size_t)n) != n) {

            err = errno ? errno : EIO;
            break;
        }
    }

    if (n < 0)
        err = errno;

    close(in);

    if (close(out) != 0 && !err)
        err = errno;

    if (err)
        unlink(to);

    return err;
}

int remove_path(const char *path) {

    struct stat st;

    if (lstat(path, &st) != 0)
        return errno;

    if (S_ISDIR(st.st_mode)) {

        DIR *d = opendir(path);
        struct dirent *e;
        int err = 0;

        if (!d)
            return errno;

        while (!err && (e = readdir(d)) != NULL) {

            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;

            char p[PATH_MAX];

            path_join(p, sizeof(p), path, e->d_name);
            err = remove_path(p);
        }

        closedir(d);

        if (err)
            return err;

        return rmdir(path) == 0 ? 0 : errno;
    }

    return unlink(path) == 0 ? 0 : errno;
}

/* moves (rename, or copy then delete across disks) */
int move_path(const char *from, const char *to) {

    if (rename(from, to) == 0)
        return 0;

    if (errno != EXDEV)
        return errno;

    int err = copy_path(from, to);

    return err ? err : remove_path(from);
}

/* "notes.txt" exists -> "notes (2).txt" */
void free_name(const char *dir, const char *name, char *out, size_t size) {

    char try[PATH_MAX], base[256], ext[64] = "";
    const char *dot = strrchr(name, '.');

    path_join(out, size, dir, name);

    if (access(out, F_OK) != 0)
        return;

    if (dot && dot != name && strlen(dot) < sizeof(ext)) {

        snprintf(ext, sizeof(ext), "%s", dot);
        snprintf(base, sizeof(base), "%.*s", (int)(dot - name), name);
    }
    else
        snprintf(base, sizeof(base), "%s", name);

    for (int k = 2; k < 1000; k++) {

        char n[320];

        snprintf(n, sizeof(n), "%s (%d)%s", base, k, ext);
        path_join(try, sizeof(try), dir, n);

        if (access(try, F_OK) != 0) {

            snprintf(out, size, "%s", try);
            return;
        }
    }
}

void trash_dir(char *out, size_t size) {

    const char *home = getenv("HOME");
    char p[PATH_MAX];

    snprintf(p, sizeof(p), "%s/.local", home ? home : "/tmp");
    mkdir(p, 0700);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/share");
    mkdir(p, 0700);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/Trash");
    mkdir(p, 0700);
    snprintf(out, size, "%s", p);
}

int in_trash(const char *path) {

    char t[PATH_MAX];

    trash_dir(t, sizeof(t));

    return !strncmp(path, t, strlen(t));
}

int move_to_trash(const char *path) {

    char t[PATH_MAX], to[PATH_MAX];
    const char *name = strrchr(path, '/');

    trash_dir(t, sizeof(t));
    free_name(t, name ? name + 1 : path, to, sizeof(to));

    return move_path(path, to);
}
