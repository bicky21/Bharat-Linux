#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>

int main() {

    DIR *d = opendir("/etc/services");

    if (!d) {

        printf("No services\n");

        return 1;
    }

    struct dirent *dir;

    while ((dir = readdir(d)) != NULL) {

        if (dir->d_name[0] == '.')
            continue;

        char path[256];

        snprintf(path, sizeof(path),
                 "/etc/services/%s",
                 dir->d_name);

        FILE *f = fopen(path, "r");

        if (!f)
            continue;

        char line[256];

        while (fgets(line, sizeof(line), f)) {

            system(line);
        }

        fclose(f);

        printf("Started %s\n", dir->d_name);
    }

    closedir(d);

    return 0;
}
