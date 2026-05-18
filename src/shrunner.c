#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2)
        return 1;

    execlp(argv[1],
           argv[1],
           NULL);

    printf("Command not found: %s\n",
           argv[1]);

    return 1;
}
