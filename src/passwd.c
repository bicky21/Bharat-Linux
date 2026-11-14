/*
 * passwd           change your own password
 * passwd <user>    change another user's password (root only)
 *
 * /bin/passwd is setuid root, because only root may write /etc/shadow.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libuser.h"

int main(int argc, char *argv[]) {

    struct bharat_user me, target;

    if (!find_user(NULL, getuid(), &me)) {

        printf("passwd: you are not in /etc/passwd\n");

        return 1;
    }

    const char *name = argc > 1 ? argv[1] : me.name;

    if (!find_user(name, 0, &target)) {

        printf("passwd: no such user: %s\n", name);

        return 1;
    }

    if (getuid() != 0 && target.uid != getuid()) {

        printf("passwd: only root may change another user's password\n");

        return 1;
    }

    if (geteuid() != 0) {

        printf("passwd: not installed as setuid root\n");

        return 1;
    }

    char old[128], first[128], second[128], hash[256];

    /* a normal user must prove who they are with the old password */
    if (getuid() != 0) {

        if (!read_hidden("current password: ", old, sizeof(old)))
            return 1;

        if (!check_password(name, old)) {

            sleep(2);

            printf("passwd: wrong password\n");

            return 1;
        }
    }

    if (!read_hidden("new password: ", first, sizeof(first)) ||
        !read_hidden("repeat new password: ", second, sizeof(second)))
        return 1;

    if (strcmp(first, second) != 0) {

        printf("passwd: the two passwords are different\n");

        return 1;
    }

    if (strlen(first) < 4) {

        printf("passwd: use at least 4 characters\n");

        return 1;
    }

    if (!make_hash(first, hash, sizeof(hash)) ||
        !write_hash(name, hash)) {

        printf("passwd: could not save the new password\n");

        return 1;
    }

    printf("Password changed for %s\n", name);

    return 0;
}
