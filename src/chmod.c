#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libperm.h"

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chmod <mode> <file>\n");

        return 1;
    }

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    if (!check_perm(argv[2], user)) {

        printf("Permission denied\n");

        return 1;
    }

    /* the mode is octal, like 755 or 644 */
    char *end;
    long mode = strtol(argv[1], &end, 8);

    if (*end != 0 || end == argv[1] || mode < 0 || mode > 07777) {

        printf("Invalid mode: %s (use numbers like 755)\n", argv[1]);

        return 1;
    }

    if (chmod(argv[2], (mode_t)mode) != 0) {

        printf("chmod failed\n");

        return 1;
    }

    char full[PATH_MAX];

    full_path(argv[2], full, sizeof(full));

    FILE *f = fopen("/etc/perms", "a");

    if (f) {

        fprintf(f,
                "%s %s\n",
                full,
                argv[1]);

        fclose(f);
    }

    printf("Permissions updated\n");

    return 0;
}
