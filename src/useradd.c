/*
 * useradd [-a] <name>
 *
 * Creates a user with a home folder. -a makes the user an administrator
 * (member of "wheel": may use sudo). The new account is locked until a
 * password is set:  passwd <name>
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libaccount.h"

int main(int argc, char *argv[]) {

    int admin = 0;
    const char *name = NULL;

    for (int i = 1; i < argc; i++) {

        if (strcmp(argv[i], "-a") == 0)
            admin = 1;
        else
            name = argv[i];
    }

    if (!name) {

        printf("Usage: useradd [-a] <name>    (-a: administrator)\n");
        return 1;
    }

    if (geteuid() != 0) {

        printf("useradd: only root can add users. Try: sudo useradd %s\n", name);
        return 1;
    }

    const char *err = create_user(name, admin);

    if (err) {

        printf("useradd: %s\n", err);
        return 1;
    }

    printf("User %s created%s. Set a password with: passwd %s\n",
           name, admin ? " (administrator)" : "", name);

    return 0;
}
