#include <stdio.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chown <owner> <file>\n");

        return 1;
    }

    FILE *f = fopen("/etc/owners", "a");

    if (!f) {

        printf("Cannot open owners database\n");

        return 1;
    }

    fprintf(f,
            "%s %s\n",
            argv[2],
            argv[1]);

    fclose(f);

    printf("Ownership updated\n");

    return 0;
}
