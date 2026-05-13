#include <stdio.h>
#include "libbharat/libbharat.h"

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: cat <file>\n");

        return 1;
    }

    if (!bharat_allowed(argv[1])) {

        printf("Permission denied\n");

        bharat_log("cat permission denied");

        return 1;
    }

    FILE *f = fopen(argv[1], "r");

    if (!f) {

        printf("Cannot open file\n");

        return 1;
    }

    char c;

    while ((c = fgetc(f)) != EOF)
        putchar(c);

    fclose(f);

    bharat_log("cat executed");

    return 0;
}
