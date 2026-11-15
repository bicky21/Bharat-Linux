/*
 * bharat-install - install Bharat Linux onto a disk, in text mode.
 *
 * The same steps as the graphical installer (the "Install Bharat Linux"
 * button on the welcome screen), for the text console:
 *   sudo bharat-install
 */

#define _GNU_SOURCE
#include <stdio.h>

#include "libinstall.h"

static void ask(const char *question, const char *def, char *out, size_t size) {

    printf(def ? "%s [%s]: " : "%s: ", question, def);
    fflush(stdout);

    if (!fgets(out, (int)size, stdin))
        out[0] = 0;

    out[strcspn(out, "\r\n")] = 0;

    if (!out[0] && def)
        snprintf(out, size, "%s", def);
}

static void show(const char *step, int percent) {

    printf("\r  [%-25.*s] %3d%%  %-30s", percent / 4, "#########################", percent, step);
    fflush(stdout);
}

int main(void) {

    if (getuid() != 0) {

        fprintf(stderr, "bharat-install needs root. Use: sudo bharat-install\n");
        return 1;
    }

    if (!is_live_system()) {

        fprintf(stderr, "bharat-install works from the Bharat Linux USB stick or DVD.\n");
        return 1;
    }

    printf("\nInstall Bharat Linux\n====================\n\n");

    struct install_disk disks[16];
    int n = install_disks(disks, 16);

    if (n == 0) {

        printf("No disk to install on was found (at least %d GB, not in use).\n", MIN_DISK_MB / 1024);
        return 1;
    }

    printf("1. Where should Bharat Linux go?\n\n");

    for (int i = 0; i < n; i++)
        printf("   %d) %s\n", i + 1, disks[i].description);

    char answer[128];

    printf("\n");
    ask("Disk number (q to stop)", n == 1 ? "1" : NULL, answer, sizeof(answer));

    int pick = atoi(answer);

    if (pick < 1 || pick > n) {

        printf("Nothing changed.\n");
        return 1;
    }

    char name[64], pass[128], repeat[128], host[64];

    printf("\n2. Your account (an administrator)\n\n");

    while (1) {

        ask("User name (small letters)", NULL, name, sizeof(name));

        if (valid_username(name))
            break;

        printf("   Use small letters and digits, starting with a letter.\n");
    }

    while (1) {

        if (!read_hidden("Password (8 or more characters): ", pass, sizeof(pass)) ||
            !read_hidden("Repeat the password: ", repeat, sizeof(repeat)))
            return 1;

        const char *weak = weak_password(pass, name);

        if (strcmp(pass, repeat))
            printf("   The two passwords are different.\n");
        else if (weak)
            printf("   %s\n", weak);
        else
            break;
    }

    ask("Computer name", "bharat-pc", host, sizeof(host));

    printf("\n3. Check\n\n"
           "   Bharat Linux will be installed on %s.\n"
           "   EVERYTHING on this disk will be erased.\n"
           "   Account: %s (administrator). Computer name: %s.\n"
           "   %s\n\n",
           disks[pick - 1].description, name, host,
           started_in_uefi_mode() ? "It will start by itself (UEFI) when you switch on."
                                  : "This PC was started in old BIOS mode: switch it to UEFI to start the new system.");

    ask("Type yes to install", NULL, answer, sizeof(answer));

    if (strcmp(answer, "yes")) {

        printf("Nothing changed.\n");
        return 1;
    }

    char details[200];

    printf("\n");

    const char *err = install_system(disks[pick - 1].name, name, pass, host, show, details, sizeof(details));

    memset(pass, 0, sizeof(pass));
    memset(repeat, 0, sizeof(repeat));
    printf("\n\n");

    if (err) {

        printf("Installation failed: %s\n%s%s", err, details, details[0] ? "\n" : "");
        return 1;
    }

    printf("Bharat Linux is installed.\n"
           "Remove the USB stick or DVD, then restart: sudo reboot\n");

    return 0;
}
