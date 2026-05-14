#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: export KEY=value\n");

        return 1;
    }

    char *eq = strchr(argv[1], '=');

    if (!eq) {

        printf("Invalid format\n");

        return 1;
    }

    *eq = 0;

    char *key = argv[1];

    char *value = eq + 1;

    setenv(key, value, 1);

    printf("%s=%s\n", key, value);

    return 0;
}
