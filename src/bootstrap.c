/*
 * bootstrap - the first program of an installed Bharat Linux.
 *
 * The kernel starts it from a tiny in-memory file system (the "initrd",
 * packed into the same EFI file as the kernel). Its only job is to find
 * and open the real system disk, then hand over to /init on it:
 *
 *   1. load the drivers for the disk (SATA/AHCI, NVMe, USB, SD cards),
 *      listed in /etc/boot-modules
 *   2. wait for the disk named by root=PARTUUID=... on the kernel command
 *      line (the installer wrote the ID of the system part there)
 *   3. check its file system (e2fsck -p), like every Linux system does
 *   4. open it and start /init from it
 *
 * If something goes wrong it says what, in plain words, and offers a
 * shell instead of a frozen screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>

#include "libdisk.h"

static void say(const char *msg) {

    printf("bharat: %s\n", msg);
    fflush(stdout);
}

static void rescue(const char *why) {

    printf("\n  Bharat Linux could not start:\n  %s\n\n"
           "  A shell follows (type reboot -f to restart).\n\n", why);
    fflush(stdout);

    execl("/bin/sh", "sh", NULL);

    /* no shell in the initrd: wait, then restart */
    sleep(60);
    reboot(RB_AUTOBOOT);
    _exit(1);
}

/* the value of NAME= on the kernel command line */
static int cmdline_value(const char *name, char *out, size_t size) {

    char line[2048] = "";
    FILE *f = fopen("/proc/cmdline", "r");

    if (f) {

        if (!fgets(line, sizeof(line), f))
            line[0] = 0;

        fclose(f);
    }

    size_t n = strlen(name);

    for (char *w = strtok(line, " \n"); w; w = strtok(NULL, " \n")) {

        if (!strncmp(w, name, n) && w[n] == '=') {

            snprintf(out, size, "%s", w + n + 1);
            return 0;
        }
    }

    return -1;
}

static void load_drivers(void) {

    FILE *f = fopen("/etc/boot-modules", "r");
    char name[64];

    while (f && fgets(name, sizeof(name), f)) {

        name[strcspn(name, " \n#")] = 0;

        if (!name[0])
            continue;

        char *argv[] = {"/bin/modprobe", "-q", name, NULL};

        run_program(argv);
    }

    if (f)
        fclose(f);
}

int main(void) {

    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    /* the kernel loads helper drivers (crc32c ...) by itself through this */
    int fd = open("/proc/sys/kernel/modprobe", O_WRONLY);

    if (fd >= 0) {

        if (write(fd, "/bin/modprobe", 13) != 13) {
            /* not fatal */
        }

        close(fd);
    }

    load_drivers();

    char spec[128], dev[256];

    if (cmdline_value("root", spec, sizeof(spec)) != 0)
        rescue("the command line has no root=");

    /* USB disks and some controllers take a few seconds to appear */
    int found = 0;

    for (int i = 0; i < 150 && !found; i++) {

        found = resolve_device(spec, dev, sizeof(dev)) == 0;

        if (!found) {

            if (i == 20)
                say("waiting for the system disk...");

            usleep(100 * 1000);
        }
    }

    if (!found) {

        char why[256];

        snprintf(why, sizeof(why), "the system disk (%s) was not found.", spec);
        rescue(why);
    }

    /* the same check as at every start of other Linux systems */
    struct fs_info fs;

    if (probe_fs(dev, &fs) != 0)
        rescue("the system part has no file system Bharat Linux knows.");

    if (check_fs(dev, fs.type) != 0)
        say("the system disk has errors that need a person: starting read-only. Repair with: sudo e2fsck -f");

    mkdir("/newroot", 0755);

    if (mount(dev, "/newroot", fs.type, 0, NULL) != 0 &&
        mount(dev, "/newroot", fs.type, MS_RDONLY, NULL) != 0)
        rescue("the system disk could not be opened.");

    if (access("/newroot/init", X_OK) != 0)
        rescue("there is no /init on the system disk.");

    /* hand /dev over; /proc and /sys are mounted again by the real init */
    mount("/dev", "/newroot/dev", NULL, MS_MOVE, NULL);
    umount2("/proc", MNT_DETACH);
    umount2("/sys", MNT_DETACH);

    /*
     * Make the system disk the new "/". The few files of this initrd
     * stay in memory (about 3 MB): small enough not to matter.
     */
    if (chdir("/newroot") != 0 || mount(".", "/", NULL, MS_MOVE, NULL) != 0 || chroot(".") != 0 || chdir("/") != 0)
        rescue("switching to the system disk failed.");

    execl("/init", "/init", NULL);

    rescue("/init on the system disk did not start.");

    return 1;
}
