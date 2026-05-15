#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: run <script>\n");

        return 1;
    }

    char cmd[256];

    snprintf(cmd,
             sizeof(cmd),
             "/bin/sh < %s",
             argv[1]);

    system(cmd);

    return 0;
}
