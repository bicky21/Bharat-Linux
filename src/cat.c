#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libperm.h"

int main(int argc, char *argv[]) {

    /* no file given: copy the keyboard or a pipe, so "ls | cat" works */
    if (argc == 1) {

        int ch;

        while ((ch = getchar()) != EOF)
            putchar(ch);

        return 0;
    }

    if (argc != 2) {

        printf("usage: cat [file]\n");

        return 1;
    }

    char *user = getenv("USER");

    if (!user)
        user = "unknown";

    if (!check_perm(argv[1], user)) {

        printf("Permission denied\n");

        return 1;
    }

    FILE *f = fopen(argv[1], "r");

    if (!f) {

        printf("Cannot open file\n");

        return 1;
    }

    /* int, not char: otherwise the byte 0xFF looks like end-of-file */
    int c;

    while ((c = fgetc(f)) != EOF)
        putchar(c);

    fclose(f);

    return 0;
}
