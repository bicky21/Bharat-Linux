#ifndef LIBPERM_H
#define LIBPERM_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int is_owner(const char *path,
             const char *user) {

    FILE *f = fopen("/etc/owners", "r");

    if (!f)
        return 0;

    char file[256];
    char owner[64];

    while (fscanf(f,
                  "%255s %63s",
                  file,
                  owner) == 2) {

        if (strcmp(file, path) == 0 &&
            strcmp(owner, user) == 0) {

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

int check_perm(const char *path,
               const char *user) {

    if (strcmp(user, "root") == 0)
        return 1;

    if (is_owner(path, user))
        return 1;

    return 0;
}

#endif
