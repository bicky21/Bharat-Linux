#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libperm.h"

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chown <owner> <file>\n");

        return 1;
    }

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    /* only root or the current owner may give a file away */
    if (!check_perm(argv[2], user)) {

        printf("Permission denied\n");

        return 1;
    }

    char full[PATH_MAX];

    full_path(argv[2], full, sizeof(full));

    FILE *f = fopen("/etc/owners", "a");

    if (!f) {

        printf("Cannot open owners database\n");

        return 1;
    }

    fprintf(f,
            "%s %s\n",
            full,
            argv[1]);

    fclose(f);

    printf("Ownership updated\n");

    return 0;
}
