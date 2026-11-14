/*
 * alias                    show all aliases
 * alias <name> <command>   save an alias in ~/.aliases (the shell reads it)
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {

    char *home = getenv("HOME");
    char path[256];

    snprintf(path, sizeof(path), "%s/.aliases", home ? home : "");

    if (argc == 1) {

        FILE *f = fopen(path, "r");
        char line[512];

        while (f && fgets(line, sizeof(line), f))
            printf("%s", line);

        if (f)
            fclose(f);

        return 0;
    }

    if (argc < 3) {

        printf("Usage: alias <name> <command>\n");

        return 1;
    }

    FILE *f = fopen(path, "a");

    if (!f) {

        printf("Cannot save alias\n");

        return 1;
    }

    fprintf(f, "%s", argv[1]);

    for (int i = 2; i < argc; i++)
        fprintf(f, " %s", argv[i]);

    fprintf(f, "\n");

    fclose(f);

    return 0;
}
