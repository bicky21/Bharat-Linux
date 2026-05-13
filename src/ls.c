#include <stdio.h>
#include <dirent.h>

int main(int argc, char *argv[]) {

    char *path = ".";

    if (argc > 1)
        path = argv[1];

    DIR *d = opendir(path);

    if (!d) {

        printf("Cannot open directory\n");

        return 1;
    }

    struct dirent *dir;

    while ((dir = readdir(d)) != NULL) {

        printf("%s\n", dir->d_name);
    }

    closedir(d);

    return 0;
}
