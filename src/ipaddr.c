#include <stdio.h>

int main() {

    FILE *f = popen("ip addr", "r");

    if (!f) {

        printf("Cannot read ip info\n");

        return 1;
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {

        printf("%s", line);
    }

    pclose(f);

    return 0;
}
