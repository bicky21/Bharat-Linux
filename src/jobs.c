#include <stdio.h>

int main() {

    FILE *f = fopen("/tmp/jobs", "r");

    if (!f) {

        printf("No jobs\n");

        return 1;
    }

    char line[256];

    printf("Background Jobs\n");
    printf("----------------\n");

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
