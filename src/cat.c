#include <stdio.h>

void read_stream(FILE *f) {

    int c;

    while ((c = fgetc(f)) != EOF) {

        putchar(c);
    }
}

int main(int argc, char *argv[]) {

    if (argc < 2) {

        read_stream(stdin);

        return 0;
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
