#include <stdlib.h>
#include <stdio.h>

int main() {

    char *home = getenv("HOME");
    char path[256];

    snprintf(path, sizeof(path), "%s/.history", home ? home : "");

    FILE *f = fopen(path, "r");

    if (!f) {

        printf("No history\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
