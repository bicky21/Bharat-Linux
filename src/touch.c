#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: touch <file>\n");

        return 1;
    }

    int fd = open(argv[1], O_CREAT, 0644);

    if (fd < 0) {

        printf("touch failed\n");

        return 1;
    }

    close(fd);

    return 0;
}
