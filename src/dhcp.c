#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: dhcp <interface>\n");

        return 1;
    }

    char cmd[256];

    snprintf(cmd,
             sizeof(cmd),
             "udhcpc -i %s",
             argv[1]);

    printf("Requesting DHCP lease...\n");

    int r = system(cmd);

    if (r == 0)
        printf("DHCP success\n");
    else
        printf("DHCP failed\n");

    return 0;
}
