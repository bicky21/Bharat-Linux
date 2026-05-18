#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: ifup <interface>\n");

        return 1;
    }

    char cmd[256];

    snprintf(cmd,
             sizeof(cmd),
             "ip link set %s up",
             argv[1]);

    int r = system(cmd);

    if (r == 0)
        printf("Interface enabled\n");
    else
        printf("Failed to enable interface\n");

    return 0;
}
