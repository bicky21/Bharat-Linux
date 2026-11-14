#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libperm.h"

int main(int argc, char *argv[]) {

    if (argc != 2) {

        printf("usage: touch <file>\n");

        return 1;
    }

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    if (!check_perm(argv[1], user)) {

        printf("Permission denied\n");

        return 1;
    }

    int existed = access(argv[1], F_OK) == 0;

    /* no O_TRUNC: touching an existing file must keep its content */
    int fd = open(argv[1], O_WRONLY | O_CREAT, 0644);

    if (fd < 0) {

        printf("Cannot create file\n");

        return 1;
    }

    /* update the modification time to "now" */
    futimens(fd, NULL);

    close(fd);

    if (existed)
        return 0;

    char full[PATH_MAX];

    full_path(argv[1], full, sizeof(full));

    FILE *own = fopen("/etc/owners", "a");

    if (own) {

        fprintf(own,
                "%s %s\n",
                full,
                user);

        fclose(own);
    }

    printf("File created\n");

    return 0;
}
