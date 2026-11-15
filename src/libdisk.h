#ifndef LIBDISK_H
#define LIBDISK_H

/*
 * Bharat Linux disk library.
 *
 *  - lists disks and their parts (partitions), from /sys/class/block
 *  - recognises what is on a part: ext2/3/4, FAT, swap, ISO, with its
 *    label and unique ID (UUID)
 *  - reads and writes GPT partition tables (the modern kind)
 *  - finds a part by "UUID=...", "LABEL=...", "PARTUUID=...", ...
 *  - mounts everything listed in /etc/fstab
 *
 * Used by blkid, lsblk, bdisk, bharat-disk, mount -a, init, devd and the
 * installer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <linux/fs.h>

/* ====================================================================
 * small helpers
 * ==================================================================== */

static inline long long disk_read_number(const char *path) {

    FILE *f = fopen(path, "r");
    long long v = 0;

    if (f) {

        if (fscanf(f, "%lld", &v) != 1)
            v = 0;

        fclose(f);
    }

    return v;
}

static inline void disk_read_text(const char *path, char *out, size_t size) {

    FILE *f = fopen(path, "r");

    out[0] = 0;

    if (f) {

        if (fgets(out, (int)size, f))
            out[strcspn(out, "\n")] = 0;

        fclose(f);
    }

    /* models are padded with spaces */
    size_t n = strlen(out);

    while (n > 0 && out[n - 1] == ' ')
        out[--n] = 0;
}

static inline int read_at(int fd, long long offset, void *buf, size_t len) {

    return pread(fd, buf, len, offset) == (ssize_t)len ? 0 : -1;
}

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* "1.5 GB" */
static inline void disk_size_text(long long bytes, char *out, size_t size) {

    const char *unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;

    while (v >= 1000 && u < 4) {

        v /= 1024;
        u++;
    }

    snprintf(out, size, u == 0 ? "%.0f %s" : (v < 10 ? "%.1f %s" : "%.0f %s"), v, unit[u]);
}

/* ====================================================================
 * GUIDs (the IDs inside GPT tables): stored partly little-endian
 * ==================================================================== */

/* bytes on disk -> "0FC63DAF-8483-4772-8E79-3D69D8477DE4" */
static inline void guid_text(const uint8_t g[16], char *out) {

    sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* text -> bytes on disk; returns 0 if the text was valid */
static inline int guid_parse(const char *s, uint8_t g[16]) {

    unsigned v[16];

    if (sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &v[3], &v[2], &v[1], &v[0], &v[5], &v[4], &v[7], &v[6],
               &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]) != 16)
        return -1;

    for (int i = 0; i < 16; i++)
        g[i] = (uint8_t)v[i];

    return 0;
}

/* a new random GUID (version 4) */
static inline int guid_random(uint8_t g[16]) {

    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0 || read(fd, g, 16) != 16) {

        if (fd >= 0)
            close(fd);

        return -1;
    }

    close(fd);

    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 (bytes are swapped) */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);

    return 0;
}

#define GPT_LINUX "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define GPT_EFI   "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define GPT_SWAP  "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"
#define GPT_DATA  "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"   /* also readable by Windows */

static inline const char *gpt_type_name(const char *guid) {

    if (strcasecmp(guid, GPT_LINUX) == 0) return "Linux";
    if (strcasecmp(guid, GPT_EFI) == 0) return "EFI";
    if (strcasecmp(guid, GPT_SWAP) == 0) return "swap";
    if (strcasecmp(guid, GPT_DATA) == 0) return "data";
    if (strcasecmp(guid, "E3C9E316-0B5C-4DB8-817D-F92DF00215AE") == 0) return "Windows reserved";
    if (strcasecmp(guid, "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC") == 0) return "Windows recovery";

    return "other";
}

/* ====================================================================
 * CRC32 (GPT protects its tables with it)
 * ==================================================================== */

static inline uint32_t crc32_gpt(const uint8_t *data, size_t len) {

    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {

        crc ^= data[i];

        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }

    return ~crc;
}

/* ====================================================================
 * what is on a part? (file system type, label, UUID)
 * ==================================================================== */

struct fs_info {
    char type[16];        /* "ext4", "vfat", "swap", "iso9660", "" */
    char uuid[40];
    char label[64];
};

static inline void trim_label(char *s) {

    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0))
        s[--n] = 0;
}

/*
 * Normal users may not read disks directly. devd (running as root)
 * writes what it found into this file, readable by everyone, the way
 * other Linux systems show /dev/disk/by-uuid. Lines (tab between):
 *   name  type  uuid  label  partuuid  partlabel
 */
#define DISK_CACHE "/run/blkid.cache"

static inline int cache_lookup(const char *dev, char fields[6][64]) {

    const char *name = strncmp(dev, "/dev/", 5) ? dev : dev + 5;
    FILE *f = fopen(DISK_CACHE, "r");
    char line[512];
    int found = 0;

    while (f && !found && fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;

        int k = 0;
        char *p = line;

        for (; k < 6; k++) {

            char *tab = strchr(p, '\t');

            if (tab)
                *tab = 0;

            snprintf(fields[k], 64, "%.63s", p);

            if (!tab) {

                k++;
                break;
            }

            p = tab + 1;
        }

        for (; k < 6; k++)
            fields[k][0] = 0;

        found = !strcmp(fields[0], name);
    }

    if (f)
        fclose(f);

    return found ? 0 : -1;
}

static inline int probe_fs(const char *dev, struct fs_info *info) {

    memset(info, 0, sizeof(*info));

    int fd = open(dev, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {

        char c[6][64];

        if (errno != EACCES || cache_lookup(dev, c) != 0 || !c[1][0])
            return -1;

        snprintf(info->type, sizeof(info->type), "%.15s", c[1]);
        snprintf(info->uuid, sizeof(info->uuid), "%.39s", c[2]);
        snprintf(info->label, sizeof(info->label), "%.63s", c[3]);

        return 0;
    }

    uint8_t b[4096];

    /* ext2/3/4: superblock at byte 1024, magic 0xEF53 */
    if (read_at(fd, 1024, b, 1024) == 0 && le16(b + 56) == 0xEF53) {

        uint32_t compat = le32(b + 92), incompat = le32(b + 96);

        strcpy(info->type, (incompat & 0x40) || (incompat & 0x80) || (incompat & 0x200) ? "ext4"
                           : (compat & 0x4) ? "ext3" : "ext2");
        sprintf(info->uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[104], b[105], b[106], b[107], b[108], b[109], b[110], b[111],
                b[112], b[113], b[114], b[115], b[116], b[117], b[118], b[119]);
        memcpy(info->label, b + 120, 16);
        trim_label(info->label);
        close(fd);
        return 0;
    }

    /* swap: "SWAPSPACE2" at the end of the first page */
    if (read_at(fd, 0, b, 4096) == 0 && memcmp(b + 4086, "SWAPSPACE2", 10) == 0) {

        strcpy(info->type, "swap");
        /* the swap UUID is stored in plain byte order */
        sprintf(info->uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[1036], b[1037], b[1038], b[1039], b[1040], b[1041], b[1042], b[1043],
                b[1044], b[1045], b[1046], b[1047], b[1048], b[1049], b[1050], b[1051]);
        memcpy(info->label, b + 1052, 16);
        trim_label(info->label);
        close(fd);
        return 0;
    }

    /* FAT: boot sector ends with 55 AA and names its type */
    if (b[510] == 0x55 && b[511] == 0xAA) {

        int fat32 = memcmp(b + 82, "FAT32", 5) == 0;
        int fat16 = memcmp(b + 54, "FAT1", 4) == 0;

        if (fat32 || fat16) {

            const uint8_t *id = b + (fat32 ? 67 : 39);

            strcpy(info->type, "vfat");
            sprintf(info->uuid, "%02X%02X-%02X%02X", id[3], id[2], id[1], id[0]);
            memcpy(info->label, b + (fat32 ? 71 : 43), 11);
            trim_label(info->label);

            if (strcmp(info->label, "NO NAME") == 0)
                info->label[0] = 0;

            close(fd);
            return 0;
        }
    }

    /* exFAT and NTFS (big USB sticks, disks from Windows) */
    if (memcmp(b + 3, "EXFAT   ", 8) == 0 || memcmp(b + 3, "NTFS    ", 8) == 0) {

        const uint8_t *id = b + (b[3] == 'E' ? 100 : 72);

        strcpy(info->type, b[3] == 'E' ? "exfat" : "ntfs3");

        if (b[3] == 'E')
            sprintf(info->uuid, "%02X%02X-%02X%02X", id[3], id[2], id[1], id[0]);
        else
            sprintf(info->uuid, "%02X%02X%02X%02X%02X%02X%02X%02X",
                    id[7], id[6], id[5], id[4], id[3], id[2], id[1], id[0]);

        close(fd);
        return 0;
    }

    /* ISO 9660 (CD/DVD, and our installer image): "CD001" at 32769 */
    if (read_at(fd, 32768, b, 2048) == 0 && memcmp(b + 1, "CD001", 5) == 0) {

        strcpy(info->type, "iso9660");
        memcpy(info->label, b + 40, 32);
        trim_label(info->label);
        close(fd);
        return 0;
    }

    close(fd);

    return -1;
}

/* ====================================================================
 * GPT partition tables
 * ==================================================================== */

#define GPT_ENTRIES 128
#define GPT_ENTRY_SIZE 128

struct gpt_part {
    int used;
    char type[40];        /* GUID text */
    char uuid[40];        /* PARTUUID */
    char name[37];        /* PARTLABEL */
    uint64_t first, last; /* sectors */
};

struct gpt {
    int sector;           /* bytes per sector (512 or 4096) */
    uint64_t sectors;     /* size of the disk in sectors */
    char disk_uuid[40];
    uint64_t first_usable, last_usable;
    struct gpt_part part[GPT_ENTRIES];
};

static inline int disk_sector_size(int fd) {

    int s = 512;

    if (ioctl(fd, BLKSSZGET, &s) != 0 || s < 512)
        s = 512;

    return s;
}

static inline uint64_t disk_bytes(int fd) {

    uint64_t bytes = 0;

    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {

        struct stat st;

        /* a plain file (a test image) instead of a real disk */
        if (fstat(fd, &st) == 0)
            bytes = (uint64_t)st.st_size;
    }

    return bytes;
}

/* Reads the GPT. Returns 0 if there is one, 1 if the disk has none. */
static inline int gpt_read(int fd, struct gpt *t) {

    memset(t, 0, sizeof(*t));

    t->sector = disk_sector_size(fd);
    t->sectors = disk_bytes(fd) / (uint64_t)t->sector;

    uint8_t *h = calloc(1, (size_t)t->sector);

    if (!h || read_at(fd, t->sector, h, (size_t)t->sector) != 0 || memcmp(h, "EFI PART", 8) != 0) {

        free(h);
        return 1;
    }

    uint32_t header_size = le32(h + 12);

    if (header_size < 92 || header_size > (uint32_t)t->sector) {

        free(h);
        return 1;
    }

    uint32_t stored = le32(h + 16);

    put32(h + 16, 0);

    if (crc32_gpt(h, header_size) != stored) {

        free(h);
        return 1;       /* damaged: treat as no table (bdisk will say so) */
    }

    t->first_usable = le64(h + 40);
    t->last_usable = le64(h + 48);
    guid_text(h + 56, t->disk_uuid);

    uint64_t entries_lba = le64(h + 72);
    uint32_t count = le32(h + 80), esize = le32(h + 84);

    free(h);

    if (esize != GPT_ENTRY_SIZE || count == 0 || count > GPT_ENTRIES)
        return 1;

    uint8_t e[GPT_ENTRIES * GPT_ENTRY_SIZE];

    if (read_at(fd, (long long)(entries_lba * (uint64_t)t->sector), e, count * esize) != 0)
        return 1;

    for (uint32_t i = 0; i < count; i++) {

        uint8_t *p = e + i * esize;
        static const uint8_t zero[16];

        if (memcmp(p, zero, 16) == 0)
            continue;

        struct gpt_part *q = &t->part[i];

        q->used = 1;
        guid_text(p, q->type);
        guid_text(p + 16, q->uuid);
        q->first = le64(p + 32);
        q->last = le64(p + 40);

        /* the name is UTF-16; keep the plain-ASCII part */
        for (int k = 0; k < 36; k++) {

            uint16_t c = le16(p + 56 + 2 * k);

            if (c == 0)
                break;

            q->name[k] = c < 128 ? (char)c : '?';
        }
    }

    return 0;
}

/* Writes the GPT (main copy at the start, backup copy at the end). */
static inline int gpt_write(int fd, struct gpt *t) {

    int ss = t->sector;
    uint64_t last = t->sectors - 1;
    uint32_t entry_sectors = (GPT_ENTRIES * GPT_ENTRY_SIZE + ss - 1) / ss;
    uint8_t e[GPT_ENTRIES * GPT_ENTRY_SIZE];

    memset(e, 0, sizeof(e));

    for (int i = 0; i < GPT_ENTRIES; i++) {

        struct gpt_part *q = &t->part[i];
        uint8_t *p = e + i * GPT_ENTRY_SIZE;

        if (!q->used)
            continue;

        if (guid_parse(q->type, p) != 0 || guid_parse(q->uuid, p + 16) != 0)
            return -1;

        put64(p + 32, q->first);
        put64(p + 40, q->last);

        for (int k = 0; k < 36 && q->name[k]; k++)
            put16(p + 56 + 2 * k, (uint8_t)q->name[k]);
    }

    uint32_t entries_crc = crc32_gpt(e, sizeof(e));
    uint8_t *h = calloc(1, (size_t)ss);
    uint8_t *mbr = calloc(1, (size_t)ss);

    if (!h || !mbr) {

        free(h);
        free(mbr);
        return -1;
    }

    /* a "protective MBR": tells old tools the whole disk is in use */
    mbr[446 + 4] = 0xEE;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, t->sectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(t->sectors - 1));
    mbr[446 + 2] = 0x02;
    mbr[446 + 5] = 0xFF;
    mbr[446 + 6] = 0xFF;
    mbr[446 + 7] = 0xFF;
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    int failed = 0;

    for (int copy = 0; copy < 2 && !failed; copy++) {

        uint64_t me = copy == 0 ? 1 : last;
        uint64_t other = copy == 0 ? last : 1;
        uint64_t entries = copy == 0 ? 2 : last - entry_sectors;

        memset(h, 0, (size_t)ss);
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, me);
        put64(h + 32, other);
        put64(h + 40, t->first_usable);
        put64(h + 48, t->last_usable);
        guid_parse(t->disk_uuid, h + 56);
        put64(h + 72, entries);
        put32(h + 80, GPT_ENTRIES);
        put32(h + 84, GPT_ENTRY_SIZE);
        put32(h + 88, entries_crc);
        put32(h + 16, crc32_gpt(h, 92));

        failed |= pwrite(fd, e, sizeof(e), (off_t)(entries * (uint64_t)ss)) != (ssize_t)sizeof(e);
        failed |= pwrite(fd, h, (size_t)ss, (off_t)(me * (uint64_t)ss)) != ss;
    }

    if (!failed)
        failed |= pwrite(fd, mbr, (size_t)ss, 0) != ss;

    free(h);
    free(mbr);

    if (failed || fsync(fd) != 0)
        return -1;

    /* ask the kernel to read the new table (fails quietly on image files) */
    ioctl(fd, BLKRRPART, 0);

    return 0;
}

/* An empty table for the whole disk. */
static inline int gpt_new(int fd, struct gpt *t) {

    memset(t, 0, sizeof(*t));

    t->sector = disk_sector_size(fd);
    t->sectors = disk_bytes(fd) / (uint64_t)t->sector;

    uint32_t entry_sectors = (GPT_ENTRIES * GPT_ENTRY_SIZE + t->sector - 1) / t->sector;

    if (t->sectors < 4096)
        return -1;

    t->first_usable = 2 + entry_sectors;
    t->last_usable = t->sectors - 2 - entry_sectors;

    uint8_t g[16];

    if (guid_random(g) != 0)
        return -1;

    guid_text(g, t->disk_uuid);

    return 0;
}

/* sectors in 1 MiB: every part starts on a 1 MiB line (fast on SSDs) */
static inline uint64_t mib_sectors(const struct gpt *t) {

    return (1024 * 1024) / (uint64_t)t->sector;
}

/*
 * Adds a part of `bytes` (0 = all free space) in the biggest free gap.
 * Returns its number (1, 2, ...) or -1.
 */
static inline int gpt_add(struct gpt *t, uint64_t bytes, const char *type, const char *name) {

    /* find the biggest free gap, starting on a 1 MiB line */
    uint64_t align = mib_sectors(t);
    uint64_t best_start = 0, best_len = 0;
    uint64_t pos = (t->first_usable + align - 1) / align * align;

    while (pos <= t->last_usable) {

        /* the next used part at or after pos */
        uint64_t next = t->last_usable + 1;
        uint64_t skip_to = 0;

        for (int i = 0; i < GPT_ENTRIES; i++) {

            struct gpt_part *q = &t->part[i];

            if (!q->used)
                continue;

            if (q->first <= pos && q->last >= pos)
                skip_to = q->last + 1;
            else if (q->first > pos && q->first < next)
                next = q->first;
        }

        if (skip_to) {

            pos = (skip_to + align - 1) / align * align;
            continue;
        }

        if (next - pos > best_len) {

            best_len = next - pos;
            best_start = pos;
        }

        pos = (next + align - 1) / align * align;

        if (next > t->last_usable)
            break;
    }

    uint64_t want = bytes ? (bytes + (uint64_t)t->sector - 1) / (uint64_t)t->sector : best_len;

    if (best_len == 0 || want > best_len || want < align)
        return -1;

    for (int i = 0; i < GPT_ENTRIES; i++) {

        struct gpt_part *q = &t->part[i];

        if (q->used)
            continue;

        uint8_t g[16];

        if (guid_random(g) != 0)
            return -1;

        memset(q, 0, sizeof(*q));
        q->used = 1;
        snprintf(q->type, sizeof(q->type), "%s", type);
        guid_text(g, q->uuid);
        snprintf(q->name, sizeof(q->name), "%s", name ? name : "");
        q->first = best_start;
        q->last = best_start + want - 1;

        return i + 1;
    }

    return -1;
}

/* ====================================================================
 * disks and parts on this computer
 * ==================================================================== */

struct blockdev {
    char name[32];          /* sda, sda1, vda, nvme0n1p2 */
    char parent[32];        /* the disk of a part ("" for a disk) */
    int partno;             /* 1, 2, ... for parts */
    long long bytes;
    int removable, readonly;
    char model[64];
};

/* Fills list with the disks and parts. Returns how many. */
static inline int list_blockdevs(struct blockdev *list, int max) {

    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL && n < max) {

        const char *nm = e->d_name;

        if (nm[0] == '.' || !strncmp(nm, "loop", 4) || !strncmp(nm, "ram", 3) ||
            !strncmp(nm, "zram", 4) || !strncmp(nm, "dm-", 3))
            continue;

        struct blockdev *b = &list[n];
        char path[300], link[PATH_MAX];

        memset(b, 0, sizeof(*b));
        snprintf(b->name, sizeof(b->name), "%.31s", nm);

        snprintf(path, sizeof(path), "/sys/class/block/%s/size", nm);
        b->bytes = disk_read_number(path) * 512;

        snprintf(path, sizeof(path), "/sys/class/block/%s/partition", nm);
        b->partno = (int)disk_read_number(path);

        if (b->partno) {

            /* /sys/class/block/sda1 -> .../block/sda/sda1: the parent is sda */
            snprintf(path, sizeof(path), "/sys/class/block/%s", nm);

            ssize_t k = readlink(path, link, sizeof(link) - 1);

            if (k > 0) {

                link[k] = 0;

                char *last = strrchr(link, '/');

                if (last) {

                    *last = 0;

                    char *parent = strrchr(link, '/');

                    snprintf(b->parent, sizeof(b->parent), "%.31s", parent ? parent + 1 : link);
                }
            }
        }

        const char *disk = b->partno ? b->parent : b->name;

        snprintf(path, sizeof(path), "/sys/block/%s/removable", disk);
        b->removable = (int)disk_read_number(path);
        snprintf(path, sizeof(path), "/sys/class/block/%s/ro", nm);
        b->readonly = (int)disk_read_number(path);
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
        disk_read_text(path, b->model, sizeof(b->model));

        n++;
    }

    if (d)
        closedir(d);

    /* sort by name: sda, sda1, sda2, sdb ... */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(list[j - 1].name, list[j].name) > 0; j--) {

            struct blockdev tmp = list[j];

            list[j] = list[j - 1];
            list[j - 1] = tmp;
        }

    return n;
}

/* the GPT name and ID of a part (from its disk's table) */
static inline void part_ids(const struct blockdev *b, char *partuuid, char *partlabel) {

    partuuid[0] = partlabel[0] = 0;

    if (!b->partno)
        return;

    char path[64];
    struct gpt t;

    snprintf(path, sizeof(path), "/dev/%.31s", b->parent);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {

        char c[6][64];
        char dev[64];

        snprintf(dev, sizeof(dev), "/dev/%.31s", b->name);

        if (errno == EACCES && cache_lookup(dev, c) == 0) {

            snprintf(partuuid, 40, "%.39s", c[4]);
            snprintf(partlabel, 40, "%.39s", c[5]);
        }

        return;
    }

    if (gpt_read(fd, &t) == 0 && b->partno <= GPT_ENTRIES && t.part[b->partno - 1].used) {

        strcpy(partuuid, t.part[b->partno - 1].uuid);
        strcpy(partlabel, t.part[b->partno - 1].name);
    }

    close(fd);
}

/* the kernel's number for a device ("8:1"), from /sys: needs no rights */
static inline int device_numbers(const char *dev, unsigned *major, unsigned *minor) {

    char path[128], text[32];
    const char *name = strncmp(dev, "/dev/", 5) ? NULL : dev + 5;

    if (name) {

        snprintf(path, sizeof(path), "/sys/class/block/%.60s/dev", name);
        disk_read_text(path, text, sizeof(text));

        if (sscanf(text, "%u:%u", major, minor) == 2)
            return 0;
    }

    /* not a disk in /sys (a file, a loop device by another name) */
    struct stat st;

    if (stat(dev, &st) != 0 || !S_ISBLK(st.st_mode))
        return -1;

    *major = major(st.st_rdev);
    *minor = minor(st.st_rdev);

    return 0;
}

/*
 * Where a device is mounted ("" if not), or "[SWAP]".
 * /proc/self/mountinfo is used, not /proc/mounts: the disk the system
 * started from is listed there as "/dev/root", which matches no name.
 * The device numbers always match.
 */
static inline void mount_point_of(const char *dev, char *out, size_t size) {

    unsigned want_major, want_minor;
    char line[1024];

    out[0] = 0;

    if (device_numbers(dev, &want_major, &want_minor) == 0) {

        FILE *f = fopen("/proc/self/mountinfo", "r");

        while (f && fgets(line, sizeof(line), f)) {

            unsigned ma, mi;
            char dir[512];

            if (sscanf(line, "%*d %*d %u:%u %*s %511s", &ma, &mi, dir) == 3 &&
                ma == want_major && mi == want_minor) {

                snprintf(out, size, "%.255s", dir);
                break;
            }
        }

        if (f)
            fclose(f);
    }

    /* in use as swap? */
    if (!out[0]) {

        FILE *f = fopen("/proc/swaps", "r");

        while (f && fgets(line, sizeof(line), f)) {

            if (strncmp(line, dev, strlen(dev)) == 0 && line[strlen(dev)] == ' ') {

                snprintf(out, size, "[SWAP]");
                break;
            }
        }

        if (f)
            fclose(f);
    }
}

/* devd: writes DISK_CACHE (see above) */
static inline void write_disk_cache(void) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);
    FILE *f = fopen(DISK_CACHE ".new", "w");

    if (!f)
        return;

    for (int i = 0; i < n; i++) {

        char dev[64], pu[40], pl[40];
        struct fs_info fs;

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);

        if (probe_fs(dev, &fs) != 0)
            memset(&fs, 0, sizeof(fs));

        part_ids(&list[i], pu, pl);

        /* tabs and new lines in a label would break the format */
        for (char *c = fs.label; *c; c++)
            if (*c == '\t' || *c == '\n')
                *c = ' ';

        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", list[i].name, fs.type, fs.uuid, fs.label, pu, pl);
    }

    fclose(f);
    chmod(DISK_CACHE ".new", 0644);
    rename(DISK_CACHE ".new", DISK_CACHE);
}

/* Is the disk (or any of its parts) in use? Then it must not be changed. */
static inline int disk_in_use(const char *disk) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    for (int i = 0; i < n; i++) {

        if (strcmp(list[i].name, disk) != 0 && strcmp(list[i].parent, disk) != 0)
            continue;

        char dev[64], mp[256];

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);
        mount_point_of(dev, mp, sizeof(mp));

        if (mp[0])
            return 1;
    }

    return 0;
}

/*
 * "UUID=...", "LABEL=...", "PARTUUID=...", "PARTLABEL=..." or "/dev/sdb1"
 * -> "/dev/sdb1". Returns 0 if found.
 */
static inline int resolve_device(const char *spec, char *out, size_t size) {

    if (spec[0] == '/') {

        snprintf(out, size, "%s", spec);
        return access(spec, F_OK) == 0 ? 0 : -1;
    }

    const char *eq = strchr(spec, '=');

    if (!eq)
        return -1;

    size_t keylen = (size_t)(eq - spec);
    const char *want = eq + 1;
    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    for (int i = 0; i < n; i++) {

        char dev[64];
        struct fs_info fs;
        char partuuid[40], partlabel[40];
        const char *have = NULL;

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);

        if (!strncmp(spec, "UUID", keylen) && keylen == 4) {

            if (probe_fs(dev, &fs) == 0) have = fs.uuid;
        }
        else if (!strncmp(spec, "LABEL", keylen) && keylen == 5) {

            if (probe_fs(dev, &fs) == 0) have = fs.label;
        }
        else if (!strncmp(spec, "PARTUUID", keylen) && keylen == 8) {

            part_ids(&list[i], partuuid, partlabel);
            have = partuuid;
        }
        else if (!strncmp(spec, "PARTLABEL", keylen) && keylen == 9) {

            part_ids(&list[i], partuuid, partlabel);
            have = partlabel;
        }

        if (have && have[0] && strcasecmp(have, want) == 0) {

            snprintf(out, size, "%s", dev);
            return 0;
        }
    }

    return -1;
}

/* ====================================================================
 * mounting
 * ==================================================================== */

/* "defaults,ro,nosuid,uid=1000" -> mount flags + the rest as text */
static inline unsigned long parse_mount_options(const char *opts, char *data, size_t size,
                                                int *nofail, int *noauto) {

    unsigned long flags = 0;
    char copy[512];

    data[0] = 0;
    *nofail = *noauto = 0;

    snprintf(copy, sizeof(copy), "%s", opts ? opts : "defaults");

    for (char *o = strtok(copy, ","); o; o = strtok(NULL, ",")) {

        if (!strcmp(o, "defaults") || !strcmp(o, "rw") || !strcmp(o, "auto")) continue;
        if (!strcmp(o, "ro")) { flags |= MS_RDONLY; continue; }
        if (!strcmp(o, "nosuid")) { flags |= MS_NOSUID; continue; }
        if (!strcmp(o, "nodev")) { flags |= MS_NODEV; continue; }
        if (!strcmp(o, "noexec")) { flags |= MS_NOEXEC; continue; }
        if (!strcmp(o, "noatime")) { flags |= MS_NOATIME; continue; }
        if (!strcmp(o, "relatime")) { flags |= MS_RELATIME; continue; }
        if (!strcmp(o, "sync")) { flags |= MS_SYNCHRONOUS; continue; }
        if (!strcmp(o, "remount")) { flags |= MS_REMOUNT; continue; }
        if (!strcmp(o, "bind")) { flags |= MS_BIND; continue; }
        if (!strcmp(o, "nofail")) { *nofail = 1; continue; }
        if (!strcmp(o, "noauto")) { *noauto = 1; continue; }
        if (!strcmp(o, "user") || !strcmp(o, "users") || !strncmp(o, "x-", 2)) continue;

        if (data[0])
            strncat(data, ",", size - strlen(data) - 1);

        strncat(data, o, size - strlen(data) - 1);
    }

    return flags;
}

static inline void make_dirs(const char *path) {

    char p[PATH_MAX];

    snprintf(p, sizeof(p), "%s", path);

    for (char *s = p + 1; *s; s++) {

        if (*s == '/') {

            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    }

    mkdir(p, 0755);
}

/* runs a program and waits; returns its exit code (or -1) */
static inline int run_program(char *const argv[]) {

    pid_t pid = fork();

    if (pid == 0) {

        execv(argv[0], argv);
        _exit(127);
    }

    int st;

    if (pid < 0 || waitpid(pid, &st, 0) < 0)
        return -1;

    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/*
 * Checks a file system before mounting it (like fsck at start-up).
 * Returns 0 if it is fine or was repaired, 1 if it needs a person.
 */
static inline int check_fs(const char *dev, const char *type) {

    if (!strncmp(type, "ext", 3) && access("/bin/e2fsck", X_OK) == 0) {

        char *argv[] = {"/bin/e2fsck", "-p", (char *)dev, NULL};
        int r = run_program(argv);

        /* 0 clean, 1 repaired, 2 repaired (reboot advised) */
        return r >= 0 && r <= 2 ? 0 : 1;
    }

    if (!strcmp(type, "vfat") && access("/bin/fsck.fat", X_OK) == 0) {

        char *argv[] = {"/bin/fsck.fat", "-a", (char *)dev, NULL};
        int r = run_program(argv);

        return r == 0 || r == 1 ? 0 : 1;
    }

    return 0;
}

/*
 * Mounts everything in /etc/fstab (except "/" and noauto lines).
 * A disk that is missing or broken never stops the computer from
 * starting: it is reported and skipped. Returns how many failed.
 */
static inline int mount_fstab(int verbose, int run_checks) {

    FILE *f = fopen("/etc/fstab", "r");
    char line[1024];
    int failed = 0;

    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {

        char spec[256], dir[256], type[32], opts[512] = "defaults";
        int dump = 0, pass = 0;

        if (line[0] == '#' || sscanf(line, "%255s %255s %31s %511s %d %d", spec, dir, type, opts, &dump, &pass) < 3)
            continue;

        if (!strcmp(dir, "/") || !strcmp(type, "proc") || !strcmp(type, "sysfs"))
            continue;

        char data[512], dev[256];
        int nofail, noauto;
        unsigned long flags = parse_mount_options(opts, data, sizeof(data), &nofail, &noauto);

        if (noauto)
            continue;

        int is_tmpfs = !strcmp(type, "tmpfs");

        if (!is_tmpfs && resolve_device(spec, dev, sizeof(dev)) != 0) {

            if (verbose || !nofail)
                fprintf(stderr, "fstab: %s not found%s\n", spec, nofail ? " (skipped)" : "");

            failed++;
            continue;
        }

        if (!strcmp(type, "swap")) {

            if (swapon(dev, 0) != 0 && errno != EBUSY) {

                fprintf(stderr, "fstab: swap %s: %s\n", dev, strerror(errno));
                failed++;
            }

            continue;
        }

        char already[256];

        mount_point_of(is_tmpfs ? "tmpfs" : dev, already, sizeof(already));

        if (!is_tmpfs && already[0])
            continue;

        if (run_checks && pass > 0 && !is_tmpfs && check_fs(dev, type) != 0) {

            fprintf(stderr, "fstab: %s has errors; mounting it read-only. Repair: sudo e2fsck %s\n", dev, dev);
            flags |= MS_RDONLY;
        }

        make_dirs(dir);

        if (mount(is_tmpfs ? "tmpfs" : dev, dir, type, flags, data[0] ? data : NULL) != 0) {

            fprintf(stderr, "fstab: cannot mount %s on %s: %s\n", spec, dir, strerror(errno));
            failed++;
        }
        else if (verbose) {

            printf("mounted %s on %s\n", spec, dir);
        }
    }

    fclose(f);

    return failed;
}

#endif
