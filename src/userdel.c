/*
 * userdel [-r] <name>
 *
 * Deletes a user. -r also deletes the user's home folder.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libaccount.h"

int main(int argc, char *argv[]) {

    int remove_home = 0;
    const char *name = NULL;

    for (int i = 1; i < argc; i++) {

        if (strcmp(argv[i], "-r") == 0)
            remove_home = 1;
        else
            name = argv[i];
    }

    if (!name) {

        printf("Usage: userdel [-r] <name>    (-r: also delete the home folder)\n");
        return 1;
    }

    if (geteuid() != 0) {

        printf("userdel: only root can delete users. Try: sudo userdel %s\n", name);
        return 1;
    }

    /* never remove the last administrator: nobody could manage the system */
    if (is_admin(name) && strcmp(name, "root") != 0) {

        int admins = 0;
        FILE *f = fopen("/etc/passwd", "r");
        char line[512];
        struct bharat_user u;

        while (f && fgets(line, sizeof(line), f)) {

            if (parse_passwd_line(line, &u) && u.uid >= FIRST_UID && is_admin(u.name))
                admins++;
        }

        if (f)
            fclose(f);

        char root_hash[256];
        int root_usable = read_hash("root", root_hash, sizeof(root_hash)) && root_hash[0] == '$';

        if (admins <= 1 && !root_usable) {

            printf("userdel: %s is the last administrator. Make another user an\n"
                   "administrator first (usermod -aG wheel NAME).\n", name);
            return 1;
        }
    }

    const char *err = delete_user(name, remove_home);

    if (err) {

        printf("userdel: %s\n", err);
        return 1;
    }

    printf("User %s deleted%s.\n", name, remove_home ? " with the home folder" : "");

    return 0;
}
