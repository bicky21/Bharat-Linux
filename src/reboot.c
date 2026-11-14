#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/mount.h>

int main() {

    sync();

    /* make the disk read-only first so nothing is half-written */
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);

    reboot(RB_AUTOBOOT);

    printf("Only root can reboot. Use: sudo reboot\n");

    return 1;
}
