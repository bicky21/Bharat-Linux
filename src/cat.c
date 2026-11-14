#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {

    /* no file given: copy the keyboard or a pipe, so "ls | cat" works */
    if (argc == 1) {

        int ch;

        while ((ch = getchar()) != EOF)
            putchar(ch);

        return 0;
    }

    int failed = 0;

    for (int i = 1; i < argc; i++) {

        /* the kernel decides if this user may read the file */
        FILE *f = fopen(argv[i], "r");

        if (!f) {

            printf("cat: %s: %s\n", argv[i], strerror(errno));

            failed = 1;

            continue;
        }

        /* int, not char: otherwise the byte 0xFF looks like end-of-file */
        int c;

        while ((c = fgetc(f)) != EOF)
            putchar(c);

        fclose(f);
    }

    return failed;
}
