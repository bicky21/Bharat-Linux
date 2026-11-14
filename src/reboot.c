/*
 * reboot - stop every program cleanly, save to disk, then restart.
 * Only root can do this; other users: sudo reboot
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

#include "libshutdown.h"

int main() {

    if (geteuid() != 0) {

        printf("Only root can restart. Use: sudo reboot\n");
        return 1;
    }

    shut_down_cleanly();

    reboot(RB_AUTOBOOT);

    printf("reboot failed\n");

    return 1;
}
