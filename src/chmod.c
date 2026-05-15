#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chmod <mode> <file>\n");

        return 1;
    }

    FILE *f = fopen("/etc/perms", "a");

    if (!f) {

        printf("Cannot open perms database\n");

        return 1;
    }

    fprintf(f,
            "%s %s\n",
            argv[2],
            argv[1]);

    fclose(f);

    printf("Permissions updated\n");

    return 0;
}
