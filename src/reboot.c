#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

int main() {

    sync();

    reboot(RB_AUTOBOOT);

    printf("Reboot failed\n");

    return 1;
}
