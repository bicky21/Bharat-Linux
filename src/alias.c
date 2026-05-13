#include <stdio.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: alias <name> <command>\n");

        return 1;
    }

    FILE *f = fopen("/home/bicky/.aliases", "a");

    if (!f)
        return 1;

    fprintf(f, "%s %s\n", argv[1], argv[2]);

    fclose(f);

    return 0;
}
