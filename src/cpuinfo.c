#include <stdio.h>

int main() {

    FILE *f = fopen("/proc/cpuinfo", "r");

    if (!f) {

        printf("Cannot open cpuinfo\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
