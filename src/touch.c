#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {

    if (argc != 2) {

        printf("usage: touch <file>\n");

        return 1;
    }

    /* no O_TRUNC: touching an existing file must keep its content */
    int fd = open(argv[1], O_WRONLY | O_CREAT, 0644);

    if (fd < 0) {

        printf("touch: %s: %s\n", argv[1], strerror(errno));

        return 1;
    }

    /* update the modification time to "now" */
    futimens(fd, NULL);

    close(fd);

    return 0;
}
