#include <stdio.h>
#include <dirent.h>
#include <ctype.h>

int main() {

    DIR *d = opendir("/proc");

    if (!d) {

        printf("Cannot open /proc\n");

        return 1;
    }

    struct dirent *dir;

    printf("PID\n");
    printf("----\n");

    while ((dir = readdir(d)) != NULL) {

        int ok = 1;

        for (int i = 0; dir->d_name[i]; i++) {

            if (!isdigit(dir->d_name[i])) {

                ok = 0;

                break;
            }
        }

        if (ok)
            printf("%s\n", dir->d_name);
    }

    closedir(d);

    return 0;
}
