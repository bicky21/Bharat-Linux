/*
 * service            run every script in /etc/services
 * service list       only show the script names
 * service <name>     run /etc/services/<name>.sh (or .service)
 */

#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int is_script(const char *name) {

    const char *dot = strrchr(name, '.');

    return dot &&
          (strcmp(dot, ".sh") == 0 ||
           strcmp(dot, ".service") == 0);
}

int run_script(const char *name) {

    char cmd[512];

    snprintf(cmd,
             sizeof(cmd),
             "/bin/sh /etc/services/%s",
             name);

    int r = system(cmd);

    printf("%s %s\n", r == 0 ? "Started" : "Failed", name);

    return r == 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {

    int list = argc >= 2 && strcmp(argv[1], "list") == 0;

    if (argc >= 2 && !list && strcmp(argv[1], "all") != 0) {

        const char *endings[] = {".sh", ".service", ""};

        for (int i = 0; i < 3; i++) {

            char name[256];
            char path[512];

            snprintf(name, sizeof(name), "%s%s", argv[1], endings[i]);
            snprintf(path, sizeof(path), "/etc/services/%s", name);

            if (is_script(name) && access(path, R_OK) == 0)
                return run_script(name);
        }

        printf("No such service: %s\n", argv[1]);

        return 1;
    }

    DIR *d = opendir("/etc/services");

    if (!d) {

        printf("No services\n");

        return 1;
    }

    struct dirent *dir;
    int failed = 0;

    while ((dir = readdir(d)) != NULL) {

        /* skip hidden files and non-scripts such as "enabled" */
        if (dir->d_name[0] == '.' || !is_script(dir->d_name))
            continue;

        if (list)
            printf("%s\n", dir->d_name);
        else
            failed |= run_script(dir->d_name);
    }

    closedir(d);

    return failed;
}
