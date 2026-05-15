#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main() {

    FILE *f = fopen("/etc/groups", "r");

    if (!f) {

        printf("No groups database\n");

        return 1;
    }

    char user[64];
    char group[64];

    char *current = getenv("USER");

    while (fscanf(f,
                  "%63s %63s",
                  user,
                  group) == 2) {

        if (strcmp(user, current) == 0) {

            printf("%s\n", group);
        }
    }

    fclose(f);

    return 0;
}
