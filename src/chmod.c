#include <stdio.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chmod <perm> <file>\n");

        return 1;
    }

    FILE *f = fopen("/etc/permissions", "a");

    if (!f) {

        printf("Cannot open permission database\n");

        return 1;
    }

    fprintf(f, "%s %s\n", argv[2], argv[1]);

    fclose(f);

    printf("Permissions updated\n");

    return 0;
}
