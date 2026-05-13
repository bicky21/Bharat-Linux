#include <stdio.h>

int main() {

    FILE *f = fopen("/run/jobs", "r");

    if (!f) {

        printf("No jobs\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
