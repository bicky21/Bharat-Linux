/*
 * passwd               change your own password
 * passwd <user>        set another user's password (root only)
 * passwd -l <user>     lock the account   (root only)
 * passwd -u <user>     unlock the account (root only)
 * passwd -S [user]     show if the account is usable
 *
 * /bin/passwd is setuid root, because only root may write /etc/shadow.
 * Example: allow logging in as root:  sudo passwd root
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libaccount.h"

static int status(const char *name) {

    char hash[256];

    if (!read_hash(name, hash, sizeof(hash))) {

        printf("%s: no password entry\n", name);
        return 1;
    }

    if (hash[0] == '$')
        printf("%s: password set, account usable\n", name);
    else if (hash[0] == '!' && hash[1] == '$')
        printf("%s: locked (passwd -u %s unlocks it)\n", name, name);
    else
        printf("%s: no password, account cannot log in (passwd %s sets one)\n", name, name);

    return 0;
}

int main(int argc, char *argv[]) {

    if (geteuid() != 0) {

        printf("passwd: not installed as setuid root\n");
        return 1;
    }

    struct bharat_user me, target;

    if (!find_user(NULL, getuid(), &me)) {

        printf("passwd: you are not in /etc/passwd\n");
        return 1;
    }

    const char *opt = argc > 1 && argv[1][0] == '-' ? argv[1] : NULL;
    const char *name = opt ? (argc > 2 ? argv[2] : me.name) : (argc > 1 ? argv[1] : me.name);

    if (!find_user(name, 0, &target)) {

        printf("passwd: no such user: %s\n", name);
        return 1;
    }

    if (getuid() != 0 && target.uid != getuid()) {

        printf("passwd: only root may change another user. Try: sudo passwd %s\n", name);
        return 1;
    }

    if (opt && strcmp(opt, "-S") == 0)
        return status(name);

    if (opt && (strcmp(opt, "-l") == 0 || strcmp(opt, "-u") == 0)) {

        if (getuid() != 0) {

            printf("passwd: only root can lock or unlock accounts\n");
            return 1;
        }

        int lock = opt[1] == 'l';
        int r = set_locked(name, lock);

        if (r == -2) {

            printf("passwd: %s has no password yet. Set one with: passwd %s\n", name, name);
            return 1;
        }

        if (r != 0) {

            printf("passwd: could not change /etc/shadow\n");
            return 1;
        }

        auth_log("passwd: account %s: %s", lock ? "locked" : "unlocked", name);
        printf("Account %s %s.\n", name, lock ? "locked" : "unlocked");

        return 0;
    }

    if (opt) {

        printf("Usage: passwd [user] | passwd -l|-u|-S <user>\n");
        return 1;
    }

    char old[128], first[128], second[128], hash[256];

    /* a normal user must prove who they are with the current password */
    if (getuid() != 0) {

        if (!read_hidden("Current password: ", old, sizeof(old)))
            return 1;

        int ok = check_password(name, old);

        memset(old, 0, sizeof(old));

        if (!ok) {

            sleep(2);
            auth_log("passwd: wrong current password for %s", name);
            printf("passwd: wrong password\n");
            return 1;
        }
    }

    if (!read_hidden("New password: ", first, sizeof(first)) ||
        !read_hidden("Repeat new password: ", second, sizeof(second)))
        return 1;

    const char *problem = strcmp(first, second) != 0
                          ? "The two passwords are different."
                          : weak_password(first, name);

    if (problem) {

        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        printf("passwd: %s\n", problem);
        return 1;
    }

    int saved = make_hash(first, hash, sizeof(hash)) && write_hash(name, hash);

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));

    if (!saved) {

        printf("passwd: could not save the new password\n");
        return 1;
    }

    auth_log("passwd: password changed for %s (by %s)", name, me.name);
    printf("Password changed for %s\n", name);

    return 0;
}
