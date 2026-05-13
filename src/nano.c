#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: nano <file>\n");

        return 1;
    }

    FILE *f = fopen(argv[1], "w");

    if (!f) {

        printf("Cannot open file\n");

        return 1;
    }

    printf("Enter text. Type :wq to save.\n");

    char line[256];

    while (1) {

        fgets(line, sizeof(line), stdin);

        if (strcmp(line, ":wq\n") == 0)
            break;

        fputs(line, f);
    }

    fclose(f);

    return 0;
}
