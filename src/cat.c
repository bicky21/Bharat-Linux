#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int allowed(char *path) {

    FILE *f = fopen("/etc/owners", "r");

    if (!f)
        return 1;

    char file[128];
    char owner[64];

    char *user = getenv("USER");

    while (fscanf(f,
                  "%127s %63s",
                  file,
                  owner) == 2) {

        if (strcmp(file, path) == 0) {

            fclose(f);

            if (strcmp(user, owner) == 0 ||
                strcmp(user, "root") == 0)
                return 1;

            return 0;
        }
    }

    fclose(f);

    return 1;
}

void read_stream(FILE *f) {

    int c;

    while ((c = fgetc(f)) != EOF)
        putchar(c);
}

int main(int argc, char *argv[]) {

    if (argc < 2) {

        read_stream(stdin);

        return 0;
    }

    if (!allowed(argv[1])) {

        printf("Permission denied\n");

        return 1;
    }

    FILE *f = fopen(argv[1], "r");

    if (!f) {

        printf("Cannot open file\n");

        return 1;
    }

    read_stream(f);

    fclose(f);

    return 0;
}
