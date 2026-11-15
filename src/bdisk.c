/*
 * bdisk - divide a disk into parts (GPT partition tables).
 *
 *   bdisk                         list the disks
 *   bdisk show sdb                the parts of sdb and the free space
 *   bdisk new sdb                 a new, empty table (erases sdb!)
 *   bdisk add sdb 10G [TYPE] [NAME]   add a part (size or "all")
 *                                 TYPE: linux (default), swap, efi, data
 *   bdisk delete sdb 2            delete part 2
 *   bdisk undo sdb                put back the table from before the
 *                                 last change
 *
 * Every change is shown first and done only after you agree (-y agrees
 * in advance, for scripts). The old table is saved first in
 * /var/lib/bharat/disk-backup/, so "undo" can bring it back.
 * A disk in use (opened, or holding swap) is not changed.
 */

#include <stdio.h>
#include <ctype.h>
#include <linux/blkpg.h>

#include "libdisk.h"

static int yes;     /* -y */

static int usage(void) {

    printf("bdisk - divide a disk into parts\n\n"
           "  bdisk                        list the disks\n"
           "  bdisk show DISK              the parts and the free space\n"
           "  bdisk new DISK               new empty table (ERASES the disk)\n"
           "  bdisk add DISK SIZE|all [linux|swap|efi|data] [NAME]\n"
           "  bdisk delete DISK NUMBER     delete a part\n"
           "  bdisk undo DISK              put back the previous table\n\n"
           "  DISK is like sdb or /dev/sdb. SIZE is like 500M, 20G, 1T.\n"
           "  -y  do not ask before changing anything\n");
    return 1;
}

static const char *disk_name(const char *arg) {

    return strncmp(arg, "/dev/", 5) ? arg : arg + 5;
}

/* "20G" -> bytes */
static uint64_t parse_size(const char *s) {

    char *end;
    double v = strtod(s, &end);
    uint64_t unit = 1;

    switch (toupper((unsigned char)*end)) {
        case 'K': unit = 1024ULL; break;
        case 'M': unit = 1024ULL * 1024; break;
        case 'G': unit = 1024ULL * 1024 * 1024; break;
        case 'T': unit = 1024ULL * 1024 * 1024 * 1024; break;
        case 0: unit = 1024ULL * 1024; break;      /* plain numbers are MB */
        default: return 0;
    }

    return v > 0 ? (uint64_t)(v * (double)unit) : 0;
}

static const char *type_guid(const char *t) {

    if (!strcmp(t, "linux")) return GPT_LINUX;
    if (!strcmp(t, "swap")) return GPT_SWAP;
    if (!strcmp(t, "efi")) return GPT_EFI;
    if (!strcmp(t, "data")) return GPT_DATA;

    return NULL;
}

static int agree(const char *question) {

    if (yes)
        return 1;

    char answer[16] = "";

    printf("%s [y/N] ", question);
    fflush(stdout);

    if (!fgets(answer, sizeof(answer), stdin))
        return 0;

    return answer[0] == 'y' || answer[0] == 'Y';
}

static int open_disk(const char *name, int writable) {

    char path[64];

    snprintf(path, sizeof(path), "/dev/%.40s", name);

    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);

    if (fd < 0)
        fprintf(stderr, "bdisk: cannot open %s: %s\n", path, strerror(errno));

    return fd;
}

/* ---------- list ---------- */

static int list_disks(void) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    printf("%-10s %9s  %-24s %s\n", "DISK", "SIZE", "MODEL", "PARTS");

    for (int i = 0; i < n; i++) {

        if (list[i].partno)
            continue;

        char size[32];
        int parts = 0;

        for (int k = 0; k < n; k++)
            parts += !strcmp(list[k].parent, list[i].name);

        disk_size_text(list[i].bytes, size, sizeof(size));
        printf("%-10s %9s  %-24s %d%s\n", list[i].name, size,
               list[i].model[0] ? list[i].model : "-", parts,
               disk_in_use(list[i].name) ? "  (in use)" : "");
    }

    return 0;
}

/* ---------- show ---------- */

static void print_table(const char *disk, const struct gpt *t) {

    char size[32];

    disk_size_text((long long)(t->sectors * (uint64_t)t->sector), size, sizeof(size));
    printf("%s: %s, GPT table %s\n\n", disk, size, t->disk_uuid);
    printf("  %-3s %10s %10s  %-9s %s\n", "NR", "START", "SIZE", "TYPE", "NAME");

    /* parts in disk order, with the free gaps between them */
    uint64_t pos = t->first_usable;
    uint64_t mib = mib_sectors(t);

    while (1) {

        int next = -1;

        for (int i = 0; i < GPT_ENTRIES; i++)
            if (t->part[i].used && t->part[i].first >= pos &&
                (next < 0 || t->part[i].first < t->part[next].first))
                next = i;

        uint64_t end = next >= 0 ? t->part[next].first : t->last_usable + 1;

        if (end > pos && end - pos >= mib) {

            char start[32];

            disk_size_text((long long)(pos * (uint64_t)t->sector), start, sizeof(start));
            disk_size_text((long long)((end - pos) * (uint64_t)t->sector), size, sizeof(size));
            printf("  %-3s %10s %10s  %s\n", "", start, size, "(free space)");
        }

        if (next < 0)
            break;

        const struct gpt_part *q = &t->part[next];
        char start[32];

        disk_size_text((long long)(q->first * (uint64_t)t->sector), start, sizeof(start));
        disk_size_text((long long)((q->last - q->first + 1) * (uint64_t)t->sector), size, sizeof(size));
        printf("  %-3d %10s %10s  %-9s %s\n", next + 1, start, size, gpt_type_name(q->type), q->name);

        pos = q->last + 1;
    }
}

static int show(const char *disk) {

    int fd = open_disk(disk, 0);
    struct gpt t;

    if (fd < 0)
        return 1;

    if (gpt_read(fd, &t) != 0) {

        close(fd);
        printf("%s has no GPT table. Make one with: bdisk new %s\n", disk, disk);
        return 0;
    }

    close(fd);
    print_table(disk, &t);

    return 0;
}

/* ---------- backup and undo ---------- */

#define BACKUP_DIR "/var/lib/bharat/disk-backup"

/* the first and last 1 MiB hold both copies of the table */
static int backup(int fd, const char *disk) {

    uint64_t bytes = disk_bytes(fd);
    static uint8_t head[1 << 20], tail[1 << 20];

    if (bytes < 2 * sizeof(head))
        return -1;

    if (read_at(fd, 0, head, sizeof(head)) != 0 ||
        read_at(fd, (long long)(bytes - sizeof(tail)), tail, sizeof(tail)) != 0)
        return -1;

    char path[160];

    mkdir("/var/lib/bharat", 0755);
    mkdir(BACKUP_DIR, 0700);
    snprintf(path, sizeof(path), BACKUP_DIR "/%.40s.img", disk);

    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (out < 0)
        return -1;

    int ok = write(out, head, sizeof(head)) == (ssize_t)sizeof(head) &&
             write(out, tail, sizeof(tail)) == (ssize_t)sizeof(tail) && fsync(out) == 0;

    close(out);

    return ok ? 0 : -1;
}

static int undo(const char *disk) {

    char path[160];
    static uint8_t head[1 << 20], tail[1 << 20];

    snprintf(path, sizeof(path), BACKUP_DIR "/%.40s.img", disk);

    int in = open(path, O_RDONLY | O_CLOEXEC);

    if (in < 0) {

        fprintf(stderr, "bdisk: no saved table for %s\n", disk);
        return 1;
    }

    int ok = read(in, head, sizeof(head)) == (ssize_t)sizeof(head) &&
             read(in, tail, sizeof(tail)) == (ssize_t)sizeof(tail);

    close(in);

    if (!ok) {

        fprintf(stderr, "bdisk: the saved table is damaged\n");
        return 1;
    }

    if (disk_in_use(disk)) {

        fprintf(stderr, "bdisk: %s is in use. Close (umount) its parts first.\n", disk);
        return 1;
    }

    if (!agree("Put back the table saved before the last change?"))
        return 1;

    int fd = open_disk(disk, 1);

    if (fd < 0)
        return 1;

    uint64_t bytes = disk_bytes(fd);

    ok = pwrite(fd, head, sizeof(head), 0) == (ssize_t)sizeof(head) &&
         pwrite(fd, tail, sizeof(tail), (off_t)(bytes - sizeof(tail))) == (ssize_t)sizeof(tail) &&
         fsync(fd) == 0;

    ioctl(fd, BLKRRPART, 0);
    close(fd);

    printf(ok ? "Done.\n" : "bdisk: could not write the table back\n");

    return ok ? 0 : 1;
}

/* ---------- changes ---------- */

/* tells the kernel about one new part even when other parts are in use */
static void announce_part(int fd, const struct gpt *t, int nr) {

    if (ioctl(fd, BLKRRPART, 0) == 0)
        return;

    struct blkpg_partition p = {
        .start = (long long)(t->part[nr - 1].first * (uint64_t)t->sector),
        .length = (long long)((t->part[nr - 1].last - t->part[nr - 1].first + 1) * (uint64_t)t->sector),
        .pno = nr,
    };
    struct blkpg_ioctl_arg a = {.op = BLKPG_ADD_PARTITION, .datalen = sizeof(p), .data = &p};

    ioctl(fd, BLKPG, &a);
}

static int save(int fd, struct gpt *t, const char *disk) {

    if (backup(fd, disk) != 0)
        fprintf(stderr, "bdisk: (could not save a copy of the old table)\n");

    if (gpt_write(fd, t) != 0) {

        fprintf(stderr, "bdisk: writing the table failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

static int new_table(const char *disk) {

    if (disk_in_use(disk)) {

        fprintf(stderr, "bdisk: %s is in use. Close (umount) its parts first.\n", disk);
        return 1;
    }

    int fd = open_disk(disk, 1);
    struct gpt t;

    if (fd < 0)
        return 1;

    if (gpt_new(fd, &t) != 0) {

        close(fd);
        fprintf(stderr, "bdisk: %s is too small\n", disk);
        return 1;
    }

    char size[32];

    disk_size_text((long long)disk_bytes(fd), size, sizeof(size));
    printf("A new, empty table on %s (%s).\nEVERYTHING on %s will be lost.\n", disk, size, disk);

    if (!agree("Continue?")) {

        close(fd);
        return 1;
    }

    /* wipe old file system signatures at the start, so nothing old is found */
    static uint8_t zero[1 << 20];

    if (pwrite(fd, zero, sizeof(zero), 0) != (ssize_t)sizeof(zero)) {
        /* gpt_write below reports real write problems */
    }

    int r = save(fd, &t, disk);

    close(fd);

    if (!r)
        printf("Done. Add parts with: bdisk add %s SIZE\n", disk);

    return r;
}

static int add(const char *disk, const char *size_arg, const char *type, const char *name) {

    const char *guid = type_guid(type ? type : "linux");

    if (!guid) {

        fprintf(stderr, "bdisk: unknown type %s (use linux, swap, efi or data)\n", type);
        return 1;
    }

    uint64_t bytes = strcmp(size_arg, "all") ? parse_size(size_arg) : 0;

    if (strcmp(size_arg, "all") && bytes == 0) {

        fprintf(stderr, "bdisk: size must be like 500M, 20G or all\n");
        return 1;
    }

    int fd = open_disk(disk, 1);
    struct gpt t;

    if (fd < 0)
        return 1;

    if (gpt_read(fd, &t) != 0) {

        close(fd);
        fprintf(stderr, "bdisk: %s has no GPT table. Make one first: bdisk new %s\n", disk, disk);
        return 1;
    }

    int nr = gpt_add(&t, bytes, guid, name ? name : (type ? type : "linux"));

    if (nr < 0) {

        close(fd);
        fprintf(stderr, "bdisk: not enough free space on %s\n", disk);
        return 1;
    }

    char size[32];

    disk_size_text((long long)((t.part[nr - 1].last - t.part[nr - 1].first + 1) * (uint64_t)t.sector),
                   size, sizeof(size));
    printf("New part %d on %s: %s, type %s, name \"%s\".\n", nr, disk, size,
           gpt_type_name(guid), t.part[nr - 1].name);

    if (!agree("Continue?")) {

        close(fd);
        return 1;
    }

    int r = save(fd, &t, disk);

    if (!r) {

        announce_part(fd, &t, nr);
        printf("Done: /dev/%s%s%d\n", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", nr);
    }

    close(fd);

    return r;
}

static int delete(const char *disk, int nr) {

    int fd = open_disk(disk, 1);
    struct gpt t;

    if (fd < 0)
        return 1;

    if (gpt_read(fd, &t) != 0 || nr < 1 || nr > GPT_ENTRIES || !t.part[nr - 1].used) {

        close(fd);
        fprintf(stderr, "bdisk: %s has no part %d\n", disk, nr);
        return 1;
    }

    char dev[64], mp[256];

    snprintf(dev, sizeof(dev), "/dev/%.40s%s%d", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", nr);
    mount_point_of(dev, mp, sizeof(mp));

    if (mp[0]) {

        close(fd);
        fprintf(stderr, "bdisk: %s is in use (%s). Close it first.\n", dev, mp);
        return 1;
    }

    printf("Delete part %d (%s, \"%s\") on %s. The files on it will be lost.\n",
           nr, gpt_type_name(t.part[nr - 1].type), t.part[nr - 1].name, disk);

    if (!agree("Continue?")) {

        close(fd);
        return 1;
    }

    memset(&t.part[nr - 1], 0, sizeof(t.part[0]));

    int r = save(fd, &t, disk);

    if (!r && ioctl(fd, BLKRRPART, 0) != 0) {

        struct blkpg_partition p = {.pno = nr};
        struct blkpg_ioctl_arg a = {.op = BLKPG_DEL_PARTITION, .datalen = sizeof(p), .data = &p};

        ioctl(fd, BLKPG, &a);
    }

    close(fd);

    if (!r)
        printf("Done. (Changed your mind? bdisk undo %s)\n", disk);

    return r;
}

int main(int argc, char **argv) {

    int k = 1;

    for (int i = 1; i < argc; i++) {

        if (!strcmp(argv[i], "-y"))
            yes = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            return usage();
        else
            argv[k++] = argv[i];
    }

    argc = k;

    if (argc == 1 || !strcmp(argv[1], "list"))
        return list_disks();

    if (argc < 3)
        return usage();

    const char *disk = disk_name(argv[2]);

    if (!strcmp(argv[1], "show"))
        return show(disk);

    if (getuid() != 0) {

        fprintf(stderr, "bdisk: changing disks needs root. Use: sudo bdisk %s ...\n", argv[1]);
        return 1;
    }

    if (!strcmp(argv[1], "new")) return new_table(disk);
    if (!strcmp(argv[1], "undo")) return undo(disk);
    if (!strcmp(argv[1], "add") && argc >= 4) return add(disk, argv[3], argc > 4 ? argv[4] : NULL, argc > 5 ? argv[5] : NULL);
    if (!strcmp(argv[1], "delete") && argc == 4) return delete(disk, atoi(argv[3]));

    return usage();
}
