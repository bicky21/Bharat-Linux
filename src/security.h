#ifndef SECURITY_H
#define SECURITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int allowed(char *path) {

    FILE *f = fopen("/etc/owners", "r");

    if (!f)
        return 1;

    char line[256];

    char *user = getenv("USER");

    while (fgets(line, sizeof(line), f)) {

        char file[128];
        char owner[64];

        sscanf(line, "%s %s", file, owner);

        if (strncmp(path, file, strlen(file)) == 0) {

            if (strcmp(owner, user) == 0 ||
                strcmp(user, "root") == 0) {

                fclose(f);

                return 1;
            }

            fclose(f);

            return 0;
        }
    }

    fclose(f);

    return 1;
}

#endif
