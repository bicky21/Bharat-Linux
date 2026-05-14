#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc == 1) {

        FILE *f = fopen("/etc/hostname", "r");

        if (!f) {

            printf("(none)\n");

            return 1;
        }

        char host[256];

        fgets(host, sizeof(host), f);

        host[strcspn(host, "\n")] = 0;

        printf("%s\n", host);

        fclose(f);

        return 0;
    }

    FILE *f = fopen("/etc/hostname", "w");

    if (!f) {

        printf("Cannot write hostname\n");

        return 1;
    }

    fprintf(f, "%s\n", argv[1]);

    fclose(f);

    printf("Hostname set to %s\n", argv[1]);

    return 0;
}
