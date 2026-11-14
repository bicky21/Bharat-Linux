#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: run <script>\n");

        return 1;
    }

    /* the shell runs a script when it gets the file name as argument */
    execl("/bin/sh", "sh", argv[1], NULL);

    printf("Cannot start /bin/sh\n");

    return 1;
}
