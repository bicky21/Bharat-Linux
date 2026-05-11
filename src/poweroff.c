#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

int main() {

    sync();

    reboot(RB_POWER_OFF);

    printf("Poweroff failed\n");

    return 1;
}
