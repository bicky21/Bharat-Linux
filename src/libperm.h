#ifndef LIBPERM_H
#define LIBPERM_H

/*
 * Bharat-linux file ownership database.
 *
 * /etc/owners holds lines of the form:   <absolute path> <owner>
 * An entry for a directory also covers everything inside it.
 * The most specific (longest) matching entry wins; if two entries are
 * equally specific, the later line wins (chown appends new lines).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

/* Turn any path into a full path starting with '/'. */
static inline void full_path(const char *path,
                             char *out,
                             size_t size) {

    char resolved[PATH_MAX];

    if (realpath(path, resolved)) {

        snprintf(out, size, "%s", resolved);
        return;
    }

    /* the file may not exist yet: build the path by hand */
    if (path[0] == '/') {

        snprintf(out, size, "%s", path);
        return;
    }

    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "");

    snprintf(out,
             size,
             "%s/%s",
             strcmp(cwd, "/") == 0 ? "" : cwd,
             path);
}

/* Returns 1 and fills owner if some entry covers path, otherwise 0. */
static inline int find_owner(const char *path,
                             char *owner,
                             size_t size) {

    char full[PATH_MAX];

    full_path(path, full, sizeof(full));

    FILE *f = fopen("/etc/owners", "r");

    if (!f)
        return 0;

    char file[PATH_MAX];
    char name[64];

    size_t best = 0;
    int found = 0;

    while (fscanf(f,
                  "%4095s %63s",
                  file,
                  name) == 2) {

        size_t len = strlen(file);

        if (strncmp(full, file, len) != 0)
            continue;

        /* must match the whole name: /home/bi must not cover /home/bicky */
        if (full[len] != 0 &&
            full[len] != '/' &&
            strcmp(file, "/") != 0)
            continue;

        if (len >= best) {

            best = len;
            found = 1;

            snprintf(owner, size, "%s", name);
        }
    }

    fclose(f);

    return found;
}

static inline int is_owner(const char *path,
                           const char *user) {

    char owner[64];

    return find_owner(path, owner, sizeof(owner)) &&
           strcmp(owner, user) == 0;
}

/*
 * May this user use this path?
 *   root            -> always
 *   nobody owns it  -> yes
 *   somebody owns it-> only the owner
 */
static inline int check_perm(const char *path,
                             const char *user) {

    char owner[64];

    if (strcmp(user, "root") == 0)
        return 1;

    if (!find_owner(path, owner, sizeof(owner)))
        return 1;

    return strcmp(owner, user) == 0;
}

#endif
