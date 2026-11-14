#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "libuser.h"

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chown <owner> <file>\n");

        return 1;
    }

    struct bharat_user owner;

    if (!find_user(argv[1], 0, &owner)) {

        printf("chown: no such user: %s\n", argv[1]);

        return 1;
    }

    /* the kernel only lets root give a file to somebody else */
    if (chown(argv[2], owner.uid, owner.gid) != 0) {

        printf("chown: %s: %s\n", argv[2], strerror(errno));

        return 1;
    }

    return 0;
}
