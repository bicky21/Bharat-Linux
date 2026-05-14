#include <stdio.h>

int main() {

    FILE *f = fopen("/var/lib/bpm/deps.db", "r");

    if (!f) {

        printf("No dependency database\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
