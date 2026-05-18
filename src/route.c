#include <stdio.h>

int main() {

    FILE *f = fopen("/proc/net/route", "r");

    if (!f) {

        printf("Cannot open route table\n");

        return 1;
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    fclose(f);

    return 0;
}
