/*
 * Disk commands: lsblk, blkid, mkswap, swapon, swapoff, eject.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "coreutils.h"
#include "../libdisk.h"

/* ---------- lsblk ---------- */

COMMAND(lsblk) {

    int flags[128] = {0};
    char *values[128] = {0};

    if (options(argc, argv, "f", "", flags, values) < 0)
        return 1;

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    if (flags['f'])
        printf("%-12s %-8s %-16s %-36s %s\n", "NAME", "FSTYPE", "LABEL", "UUID", "MOUNTPOINT");
    else
        printf("%-12s %8s %2s %2s %-5s %s\n", "NAME", "SIZE", "RM", "RO", "TYPE", "MOUNTPOINT");

    for (int i = 0; i < n; i++) {

        struct blockdev *b = &list[i];
        char dev[64], mp[256], size[32], name[40];

        snprintf(dev, sizeof(dev), "/dev/%.31s", b->name);
        mount_point_of(dev, mp, sizeof(mp));
        snprintf(name, sizeof(name), "%s%.31s", b->partno ? "`-" : "", b->name);

        if (flags['f']) {

            struct fs_info fs;

            if (probe_fs(dev, &fs) != 0)
                memset(&fs, 0, sizeof(fs));

            printf("%-12s %-8s %-16s %-36s %s\n", name, fs.type, fs.label, fs.uuid, mp);
        }
        else {

            human(b->bytes, size, sizeof(size));
            printf("%-12s %8s %2d %2d %-5s %s\n", name, size, b->removable, b->readonly,
                   b->partno ? "part" : (!strncmp(b->name, "sr", 2) ? "rom" : "disk"), mp);
        }
    }

    return 0;
}

/* ---------- blkid ---------- */

static void blkid_one(const struct blockdev *b, const char *dev, int *found) {

    struct fs_info fs;
    char partuuid[40] = "", partlabel[40] = "";
    int has_fs = probe_fs(dev, &fs) == 0;

    if (b)
        part_ids(b, partuuid, partlabel);

    if (!has_fs && !partuuid[0])
        return;

    printf("%s:", dev);

    if (has_fs && fs.label[0]) printf(" LABEL=\"%s\"", fs.label);
    if (has_fs && fs.uuid[0]) printf(" UUID=\"%s\"", fs.uuid);
    if (has_fs) printf(" TYPE=\"%s\"", fs.type);
    if (partlabel[0]) printf(" PARTLABEL=\"%s\"", partlabel);
    if (partuuid[0]) printf(" PARTUUID=\"%s\"", partuuid);

    printf("\n");
    (*found)++;
}

COMMAND(blkid) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128), found = 0;

    /* blkid -t UUID=... or blkid LABEL=x: just print the device */
    if (argc == 2 && strchr(argv[1], '=') && argv[1][0] != '/') {

        char dev[256];

        if (resolve_device(argv[1], dev, sizeof(dev)) != 0)
            return 2;

        printf("%s\n", dev);
        return 0;
    }

    for (int i = 0; i < n; i++) {

        char dev[64];

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);

        if (argc > 1) {

            int wanted = 0;

            for (int k = 1; k < argc; k++)
                wanted |= strcmp(argv[k], dev) == 0;

            if (!wanted)
                continue;
        }

        blkid_one(&list[i], dev, &found);
    }

    /* a plain file given by name (a disk image) */
    for (int k = 1; k < argc && found == 0; k++)
        blkid_one(NULL, argv[k], &found);

    return found ? 0 : 2;
}

/* ---------- swap ---------- */

COMMAND(mkswap) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "L", "L", flags, values);

    if (i < 0 || i != argc - 1)
        return fail("usage: mkswap [-L LABEL] DEVICE-OR-FILE");

    char mp[256];

    mount_point_of(argv[i], mp, sizeof(mp));

    if (mp[0])
        return fail("%s is in use (%s)", argv[i], mp);

    int fd = open(argv[i], O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return fail("%s: %s", argv[i], strerror(errno));

    long page = sysconf(_SC_PAGESIZE);
    uint64_t bytes = disk_bytes(fd);
    uint64_t pages = bytes / (uint64_t)page;

    if (pages < 10) {

        close(fd);
        return fail("%s is too small for swap", argv[i]);
    }

    uint8_t *h = calloc(1, (size_t)page);
    uint8_t id[16];

    if (!h || guid_random(id) != 0) {

        close(fd);
        free(h);
        return fail("out of memory");
    }

    put32(h + 1024, 1);                           /* version */
    put32(h + 1028, (uint32_t)(pages - 1));       /* last page */
    put32(h + 1032, 0);                           /* bad pages */
    memcpy(h + 1036, id, 16);

    if (values['L'])
        snprintf((char *)h + 1052, 16, "%s", values['L']);

    memcpy(h + page - 10, "SWAPSPACE2", 10);

    int bad = pwrite(fd, h, (size_t)page, 0) != page || fsync(fd) != 0;

    free(h);
    close(fd);

    if (bad)
        return fail("cannot write to %s: %s", argv[i], strerror(errno));

    char size[32];

    human((long long)(pages - 1) * page, size, sizeof(size));
    printf("Swap ready on %s: %s, UUID=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
           argv[i], size, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
           id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);

    return 0;
}

static int show_swaps(void) {

    FILE *f = fopen("/proc/swaps", "r");
    char line[512];

    while (f && fgets(line, sizeof(line), f))
        fputs(line, stdout);

    if (f)
        fclose(f);

    return 0;
}

COMMAND(swapon) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "as", "", flags, values);

    if (i < 0)
        return 1;

    if (flags['s'] || (i >= argc && !flags['a']))
        return show_swaps();

    if (flags['a'])
        return mount_fstab(0, 0) ? 1 : 0;

    int failed = 0;

    for (; i < argc; i++) {

        char dev[256];

        if (resolve_device(argv[i], dev, sizeof(dev)) != 0) {

            failed = fail("%s: not found", argv[i]);
            continue;
        }

        if (swapon(dev, 0) != 0)
            failed = fail("%s: %s", dev, errno == EINVAL ? "not a swap area (use mkswap first)" : strerror(errno));
    }

    return failed;
}

COMMAND(swapoff) {

    int failed = 0;

    if (argc < 2)
        return fail("usage: swapoff DEVICE-OR-FILE...");

    for (int i = 1; i < argc; i++) {

        char dev[256];

        if (resolve_device(argv[i], dev, sizeof(dev)) != 0 || swapoff(dev) != 0)
            failed = fail("%s: %s", argv[i], strerror(errno));
    }

    return failed;
}

/* ---------- eject ---------- */

/*
 * "eject sdb", "eject /dev/sdb1" or "eject /media/ravi/PENDRIVE".
 * devd (which mounted the stick) does the work, so normal users can
 * remove their own USB sticks without sudo.
 */
COMMAND(eject) {

    if (argc != 2)
        return fail("usage: eject DISK   (for example: eject sdb, or eject /media/%s/NAME)",
                    getenv("USER") ? getenv("USER") : "you");

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};

    snprintf(a.sun_path, sizeof(a.sun_path), "/run/devd.sock");

    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0)
        return fail("the device service (devd) is not running");

    char msg[600], reply[600];

    snprintf(msg, sizeof(msg), "eject %s\n", argv[1]);

    if (write(s, msg, strlen(msg)) < 0)
        return fail("cannot talk to devd");

    ssize_t n = read(s, reply, sizeof(reply) - 1);

    close(s);

    if (n <= 0)
        return fail("no answer from devd");

    reply[n] = 0;

    if (!strncmp(reply, "ok", 2)) {

        printf("%s", reply + 3);
        return 0;
    }

    fprintf(stderr, "eject: %s", reply + (strncmp(reply, "error ", 6) ? 0 : 6));
    return 1;
}
