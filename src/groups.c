#include <stdio.h>

int main() {

    FILE *f = fopen("/etc/group", "r");

    if (!f) {

        printf("Cannot open group database\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
