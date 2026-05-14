#include <stdio.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chown <user> <file>\n");

        return 1;
    }

    FILE *f = fopen("/etc/filemeta", "a");

    if (!f)
        return 1;

    fprintf(f, "%s %s\n", argv[2], argv[1]);

    fclose(f);

    return 0;
}
