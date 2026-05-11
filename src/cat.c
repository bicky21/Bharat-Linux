#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        printf("Cannot open file\n");
        return 1;
    }

    char buffer[256];

    int bytes;

    while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) {

        write(1, buffer, bytes);
    }

    close(fd);

    return 0;
}
