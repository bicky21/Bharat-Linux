#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {

    if (argc == 1) {

        FILE *f = fopen("/etc/hostname", "r");

        if (!f) {

            printf("(none)\n");

            return 0;
        }

        char host[128] = "(none)\n";

        if (!fgets(host, sizeof(host), f))
            printf("(none)\n");
        else
            printf("%s", host);

        fclose(f);

        return 0;
    }

    FILE *f = fopen("/etc/hostname", "w");

    if (!f) {

        printf("Cannot set hostname\n");

        return 1;
    }

    fprintf(f, "%s\n", argv[1]);

    fclose(f);

    /* also tell the running kernel, not only the file */
    if (sethostname(argv[1], strlen(argv[1])) != 0)
        printf("Warning: saved to /etc/hostname, but the kernel refused it\n");

    printf("Hostname updated\n");

    return 0;
}
