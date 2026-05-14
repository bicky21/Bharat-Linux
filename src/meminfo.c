#include <stdio.h>

int main() {

    FILE *f = fopen("/proc/meminfo", "r");

    if (!f) {

        printf("Cannot read memory info\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
