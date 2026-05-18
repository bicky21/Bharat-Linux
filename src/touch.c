#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {

    if (argc != 2) {

        printf("usage: touch <file>\n");

        return 1;
    }

    FILE *f = fopen(argv[1], "w");

    if (!f) {

        printf("Cannot create file\n");

        return 1;
    }

    fclose(f);

    FILE *own = fopen("/etc/owners", "a");

    if (own) {

        char *user = getenv("USER");

        if (!user)
            user = "unknown";

        fprintf(own,
                "%s %s\n",
                argv[1],
                user);

        fclose(own);
    }

    printf("File created\n");

    return 0;
}
