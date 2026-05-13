#ifndef LIBBHARAT_H
#define LIBBHARAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* bharat_get_user() {

    char *u = getenv("USER");

    if (!u)
        return "unknown";

    return u;
}

int bharat_allowed(char *path) {

    FILE *f = fopen("/etc/owners", "r");

    if (!f)
        return 1;

    char line[256];

    char *user = bharat_get_user();

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

void bharat_log(char *msg) {

    FILE *f = fopen("/var/log/bharat.log", "a");

    if (!f)
        return;

    fprintf(f, "[%s] %s\n",
            bharat_get_user(),
            msg);

    fclose(f);
}

#endif
