#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: exists <file>\n");

        return 1;
    }

    if (access(argv[1], F_OK) == 0)
        printf("yes\n");
    else
        printf("no\n");

    return 0;
}
