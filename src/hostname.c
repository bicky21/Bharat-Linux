#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc == 1) {

        FILE *f = fopen("/etc/hostname", "r");

        if (!f) {

            printf("(none)\n");

            return 0;
        }

        char host[128];

        fgets(host, sizeof(host), f);

        fclose(f);

        printf("%s", host);

        return 0;
    }

    FILE *f = fopen("/etc/hostname", "w");

    if (!f) {

        printf("Cannot set hostname\n");

        return 1;
    }

    fprintf(f, "%s\n", argv[1]);

    fclose(f);

    printf("Hostname updated\n");

    return 0;
}
