#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {

    if (argc < 3) {

        printf("Usage: chmod <mode> <file>\n");

        return 1;
    }

    /* the mode is octal, like 755 or 644 */
    char *end;
    long mode = strtol(argv[1], &end, 8);

    if (*end != 0 || end == argv[1] || mode < 0 || mode > 07777) {

        printf("Invalid mode: %s (use numbers like 755)\n", argv[1]);

        return 1;
    }

    /* the kernel only lets the owner (or root) do this */
    if (chmod(argv[2], (mode_t)mode) != 0) {

        printf("chmod: %s: %s\n", argv[2], strerror(errno));

        return 1;
    }

    return 0;
}
