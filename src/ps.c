#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>

int main() {

    DIR *d;

    struct dirent *dir;

    d = opendir("/proc");

    if (!d) {

        printf("Cannot open /proc\n");

        return 1;
    }

    printf("PID OWNER\n");

    char *user = getenv("USER");

    while ((dir = readdir(d)) != NULL) {

        int valid = 1;

        for (int i = 0; dir->d_name[i] != '\0'; i++) {

            if (!isdigit(dir->d_name[i])) {

                valid = 0;

                break;
            }
        }

        if (valid) {

            printf("%s %s\n", dir->d_name, user);
        }
    }

    closedir(d);

    return 0;
}
