#include <stdio.h>
#include <dirent.h>

int main() {

    DIR *d;

    struct dirent *dir;

    d = opendir(".");

    if (d == NULL) {
        printf("Cannot open directory\n");
        return 1;
    }

    while ((dir = readdir(d)) != NULL) {

        printf("%s\n", dir->d_name);
    }

    closedir(d);

    return 0;
}
