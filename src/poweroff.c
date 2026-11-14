/*
 * poweroff - stop every program cleanly, save to disk, then power off.
 * Only root can do this; other users: sudo poweroff
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

#include "libshutdown.h"

int main() {

    if (geteuid() != 0) {

        printf("Only root can power off. Use: sudo poweroff\n");
        return 1;
    }

    shut_down_cleanly();

    reboot(RB_POWER_OFF);

    printf("poweroff failed\n");

    return 1;
}
