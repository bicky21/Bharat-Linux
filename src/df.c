#include <stdio.h>
#include <sys/statvfs.h>

int main() {

    struct statvfs s;

    if (statvfs("/", &s) != 0) {

        printf("Cannot read filesystem stats\n");

        return 1;
    }

    unsigned long total =
        (s.f_blocks * s.f_frsize) / 1024;

    unsigned long free =
        (s.f_bfree * s.f_frsize) / 1024;

    printf("Filesystem: /\n");

    printf("Total: %lu KB\n", total);

    printf("Free: %lu KB\n", free);

    return 0;
}
