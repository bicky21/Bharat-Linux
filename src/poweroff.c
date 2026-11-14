#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/mount.h>

int main() {

    sync();

    /* make the disk read-only first so nothing is half-written */
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);

    reboot(RB_POWER_OFF);

    printf("Poweroff failed\n");

    return 1;
}
