/*
 * usermod -aG <group> <name>    add the user to a group
 * usermod -rG <group> <name>    remove the user from a group
 * usermod -L <name>             lock the account (no login possible)
 * usermod -U <name>             unlock it
 *
 * Example: make bicky an administrator:  usermod -aG wheel bicky
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libaccount.h"

static int usage(void) {

    printf("Usage: usermod -aG <group> <name>   add to a group\n"
           "       usermod -rG <group> <name>   remove from a group\n"
           "       usermod -L <name>            lock the account\n"
           "       usermod -U <name>            unlock the account\n");
    return 1;
}

int main(int argc, char *argv[]) {

    if (argc < 3)
        return usage();

    if (geteuid() != 0) {

        printf("usermod: only root can change users. Try: sudo usermod ...\n");
        return 1;
    }

    const char *opt = argv[1];
    const char *name = argv[argc - 1];
    struct bharat_user u;

    if (!find_user(name, 0, &u)) {

        printf("usermod: no such user: %s\n", name);
        return 1;
    }

    if ((strcmp(opt, "-aG") == 0 || strcmp(opt, "-rG") == 0) && argc == 4) {

        const char *group = argv[2];
        int add = opt[1] == 'a';

        if (!group_exists(group)) {

            printf("usermod: no such group: %s\n", group);
            return 1;
        }

        if (set_group_member(group, name, add) != 0) {

            printf("usermod: could not change /etc/group\n");
            return 1;
        }

        auth_log("%s %s group %s", name, add ? "added to" : "removed from", group);
        printf("%s %s %s. It takes effect at the next login.\n",
               name, add ? "added to" : "removed from", group);

        return 0;
    }

    if ((strcmp(opt, "-L") == 0 || strcmp(opt, "-U") == 0) && argc == 3) {

        int lock = opt[1] == 'L';
        int r = set_locked(name, lock);

        if (r == -2) {

            printf("usermod: %s has no password yet. Set one with: passwd %s\n", name, name);
            return 1;
        }

        if (r != 0) {

            printf("usermod: could not change /etc/shadow\n");
            return 1;
        }

        auth_log("account %s: %s", lock ? "locked" : "unlocked", name);
        printf("Account %s %s.\n", name, lock ? "locked" : "unlocked");

        return 0;
    }

    return usage();
}
