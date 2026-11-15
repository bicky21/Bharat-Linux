/*
 * bharat-disk - add a disk to the computer, the easy way.
 *
 *   sudo bharat-disk add       guided: choose a disk, give it a name,
 *                              and it is ready every time you start
 *   bharat-disk list           the disks and what is on them
 *   sudo bharat-disk forget NAME   stop opening a disk at start-up
 *   sudo bharat-disk swapfile 1G   extra swap space in a file
 *
 * What "add" does, step by step (all shown before anything happens):
 *   1. a new GPT table with one part filling the disk
 *   2. a file system on it: ext4 (for Linux) or FAT32 (also readable by
 *      Windows, phones and cameras)
 *   3. a folder /data/NAME, owned by you
 *   4. a line in /etc/fstab, so it opens at every start. It is found by
 *      its UUID, so it still works when disks change places, and
 *      "nofail" means a missing disk never stops the computer.
 */

#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "libdisk.h"
#include "libuser.h"
#include "libaccount.h"

static void ask(const char *question, const char *def, char *out, size_t size) {

    if (def && def[0])
        printf("%s [%s]: ", question, def);
    else
        printf("%s: ", question);

    fflush(stdout);

    if (!fgets(out, (int)size, stdin))
        out[0] = 0;

    out[strcspn(out, "\r\n")] = 0;

    if (!out[0] && def)
        snprintf(out, size, "%s", def);
}

static int valid_name(const char *s) {

    if (!s[0] || strlen(s) > 11)
        return 0;

    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
            return 0;

    return 1;
}

static void part_path(const char *disk, int nr, char *out, size_t size) {

    snprintf(out, size, "/dev/%.40s%s%d", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", nr);
}

static int run(const char *what, char *const argv[]) {

    printf("  %s...\n", what);
    fflush(stdout);

    int r = run_program(argv);

    if (r != 0)
        fprintf(stderr, "bharat-disk: %s failed (%s exited with %d)\n", what, argv[0], r);

    return r;
}

/* waits up to 5 seconds for the kernel to create the device file */
static int wait_for(const char *dev) {

    for (int i = 0; i < 50; i++) {

        if (access(dev, F_OK) == 0)
            return 0;

        usleep(100 * 1000);
    }

    return -1;
}

static int list(void) {

    char *argv[] = {"/bin/lsblk", "-f", NULL};

    return run_program(argv);
}

/* ---------- add ---------- */

static int add(void) {

    struct blockdev all[128];
    struct blockdev *choice[32];
    int n = list_blockdevs(all, 128), count = 0;

    printf("\nAdd a disk\n==========\n\n"
           "Disks that are not in use:\n\n");

    for (int i = 0; i < n && count < 32; i++) {

        if (all[i].partno || all[i].readonly || !strncmp(all[i].name, "sr", 2) ||
            disk_in_use(all[i].name) || all[i].bytes < 64LL * 1024 * 1024)
            continue;

        char size[32], what[200] = "";
        int parts = 0;

        disk_size_text(all[i].bytes, size, sizeof(size));

        for (int k = 0; k < n; k++) {

            if (strcmp(all[k].parent, all[i].name))
                continue;

            char dev[64];
            struct fs_info fs;

            parts++;
            snprintf(dev, sizeof(dev), "/dev/%.31s", all[k].name);

            if (probe_fs(dev, &fs) == 0 && strlen(what) < 150) {

                char one[100];

                snprintf(one, sizeof(one), "%s%s%s%s", what[0] ? ", " : "", fs.type,
                         fs.label[0] ? " " : "", fs.label);
                strcat(what, one);
            }
        }

        if (!parts) {

            char dev[64];
            struct fs_info fs;

            snprintf(dev, sizeof(dev), "/dev/%.31s", all[i].name);

            if (probe_fs(dev, &fs) == 0)
                snprintf(what, sizeof(what), "%s %s", fs.type, fs.label);
        }

        count++;
        choice[count - 1] = &all[i];
        printf("  %d) %-8s %9s  %-20s %s\n", count, all[i].name, size,
               all[i].model[0] ? all[i].model : "",
               what[0] ? what : (parts ? "has parts" : "empty"));
    }

    if (!count) {

        printf("  (none)\n\nPlug in or attach a disk and try again.\n"
               "Disks already in use are not shown: see them with lsblk -f\n");
        return 1;
    }

    char answer[64];
    int pick = 0;

    printf("\n");
    ask("Which disk? (number, or q to stop)", count == 1 ? "1" : NULL, answer, sizeof(answer));
    pick = atoi(answer);

    if (pick < 1 || pick > count) {

        printf("Nothing changed.\n");
        return 1;
    }

    struct blockdev *d = choice[pick - 1];
    char name[32], kind[16], owner[64];
    const char *sudo_user = getenv("SUDO_USER");

    printf("\n");

    do {
        ask("A short name for it (letters, digits, - or _, up to 11)", "data", name, sizeof(name));
    } while (!valid_name(name) && printf("  That name cannot be used.\n"));

    printf("\nHow will you use it?\n"
           "  1) Only with Linux (ext4: safest, keeps owners and permissions)\n"
           "  2) Also with Windows, phones, TVs (FAT32: files up to 4 GB)\n");
    ask("Choose", "1", kind, sizeof(kind));

    int fat = kind[0] == '2';

    ask("\nWho owns it? (user name)", sudo_user && strcmp(sudo_user, "root") ? sudo_user : "root",
        owner, sizeof(owner));

    struct bharat_user u;

    if (!find_user(owner, (uid_t)-1, &u)) {

        printf("There is no user %s. Nothing changed.\n", owner);
        return 1;
    }

    char size[32], dir[80], dev[64];

    disk_size_text(d->bytes, size, sizeof(size));
    snprintf(dir, sizeof(dir), "/data/%s", name);
    part_path(d->name, 1, dev, sizeof(dev));

    printf("\nThis will happen:\n"
           "  * EVERYTHING on %s (%s%s%s) is erased\n"
           "  * it becomes one part, %s, formatted as %s, named %s\n"
           "  * it opens at %s, owned by %s, every time the computer starts\n\n",
           d->name, size, d->model[0] ? " " : "", d->model, dev, fat ? "FAT32" : "ext4", name, dir, owner);

    ask("Type yes to go ahead", NULL, answer, sizeof(answer));

    if (strcmp(answer, "yes")) {

        printf("Nothing changed.\n");
        return 1;
    }

    printf("\n");

    /* 1. the table */
    char *table[] = {"/bin/bdisk", "-y", "new", d->name, NULL};
    char *part[] = {"/bin/bdisk", "-y", "add", d->name, "all", fat ? "data" : "linux", name, NULL};
    int quiet = open("/dev/null", O_WRONLY), saved = dup(STDOUT_FILENO);

    printf("  Making the table...\n");
    fflush(stdout);
    dup2(quiet, STDOUT_FILENO);

    int r = run_program(table) || run_program(part);

    dup2(saved, STDOUT_FILENO);
    close(quiet);
    close(saved);

    if (r || wait_for(dev) != 0) {

        fprintf(stderr, "bharat-disk: the new part %s did not appear\n", dev);
        return 1;
    }

    /* 2. the file system */
    char *mkfs_ext4[] = {"/bin/mkfs.ext4", "-q", "-F", "-L", name, "-m", "1", dev, NULL};
    char *mkfs_fat[] = {"/bin/mkfs.fat", "-F", "32", "-n", name, dev, NULL};

    if (access(fat ? "/bin/mkfs.fat" : "/bin/mkfs.ext4", X_OK) != 0) {

        fprintf(stderr, "bharat-disk: the formatting tool is missing (rebuild with: make third-party)\n");
        return 1;
    }

    /* FAT labels are UPPER CASE */
    if (fat)
        for (char *c = name; *c; c++)
            *c = (char)toupper((unsigned char)*c);

    if (run("Formatting (this can take a minute)", fat ? mkfs_fat : mkfs_ext4))
        return 1;

    struct fs_info fs;

    if (probe_fs(dev, &fs) != 0) {

        fprintf(stderr, "bharat-disk: cannot read the new file system on %s\n", dev);
        return 1;
    }

    /* 3. the folder */
    char opts[128];

    if (fat)
        snprintf(opts, sizeof(opts), "uid=%u,gid=%u,fmask=0077,dmask=0077,nosuid,nodev,nofail",
                 (unsigned)u.uid, (unsigned)u.gid);
    else
        snprintf(opts, sizeof(opts), "defaults,nosuid,nodev,nofail");

    char line[300], data[256];
    int nofail, noauto;
    unsigned long flags = parse_mount_options(opts, data, sizeof(data), &nofail, &noauto);

    printf("  Opening it at %s...\n", dir);
    make_dirs(dir);

    if (mount(dev, dir, fat ? "vfat" : "ext4", flags, data[0] ? data : NULL) != 0) {

        fprintf(stderr, "bharat-disk: cannot open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    if (!fat && (chown(dir, u.uid, u.gid) != 0 || chmod(dir, 0750) != 0))
        fprintf(stderr, "bharat-disk: could not give %s to %s\n", dir, owner);

    /* 4. fstab */
    snprintf(line, sizeof(line), "UUID=%s %s %s %s 0 %d", fs.uuid, dir, fat ? "vfat" : "ext4", opts, fat ? 0 : 2);
    printf("  Adding it to /etc/fstab...\n");

    if (rewrite_file("/etc/fstab", 0644, NULL, NULL, line) != 0) {

        fprintf(stderr, "bharat-disk: could not change /etc/fstab\n");
        return 1;
    }

    printf("\nDone! %s is ready at %s\n", name, dir);

    return 0;
}

/* ---------- forget ---------- */

static int drop_line(char *line, void *ctx) {

    char spec[256], dir[256];

    if (line[0] == '#' || sscanf(line, "%255s %255s", spec, dir) != 2)
        return 1;

    return strcmp(dir, (const char *)ctx) != 0;
}

static int forget(const char *name) {

    char dir[300];

    snprintf(dir, sizeof(dir), name[0] == '/' ? "%s" : "/data/%s", name);

    if (!strcmp(dir, "/") || !strcmp(dir, "/boot/efi")) {

        fprintf(stderr, "bharat-disk: %s is needed by the system\n", dir);
        return 1;
    }

    if (umount(dir) != 0 && errno == EBUSY) {

        fprintf(stderr, "bharat-disk: %s is in use: close the files open on it first\n", dir);
        return 1;
    }

    if (rewrite_file("/etc/fstab", 0644, drop_line, dir, NULL) != 0) {

        fprintf(stderr, "bharat-disk: could not change /etc/fstab\n");
        return 1;
    }

    rmdir(dir);
    printf("%s will no longer open at start-up. The files on the disk are untouched.\n", dir);

    return 0;
}

/* ---------- swapfile ---------- */

static int swapfile(const char *size_arg) {

    char *end;
    double v = strtod(size_arg, &end);
    uint64_t unit = (*end == 'G' || *end == 'g') ? 1ULL << 30 : 1ULL << 20;
    uint64_t bytes = (uint64_t)(v * (double)unit);

    if (bytes < (16ULL << 20)) {

        fprintf(stderr, "bharat-disk: swap file size must be like 512M or 2G\n");
        return 1;
    }

    const char *path = "/swapfile";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

    if (fd < 0) {

        fprintf(stderr, "bharat-disk: %s: %s\n", path, strerror(errno));
        return 1;
    }

    /* swap files must have real blocks (no holes), so write zeros */
    static char zero[1 << 20];

    printf("Writing %s...\n", path);

    for (uint64_t done = 0; done < bytes; done += sizeof(zero)) {

        if (write(fd, zero, sizeof(zero)) != (ssize_t)sizeof(zero)) {

            fprintf(stderr, "bharat-disk: not enough space: %s\n", strerror(errno));
            close(fd);
            unlink(path);
            return 1;
        }
    }

    fsync(fd);
    close(fd);

    char *mk[] = {"/bin/mkswap", (char *)path, NULL};

    if (run_program(mk) != 0 || swapon(path, 0) != 0 ||
        rewrite_file("/etc/fstab", 0644, NULL, NULL, "/swapfile none swap defaults,nofail 0 0") != 0) {

        fprintf(stderr, "bharat-disk: could not turn on the swap file\n");
        return 1;
    }

    printf("Done: %s is used as swap, now and at every start.\n", path);

    return 0;
}

int main(int argc, char **argv) {

    if (argc < 2 || !strcmp(argv[1], "list"))
        return argc < 2 ? (printf("bharat-disk - disks made easy\n\n"
                                  "  sudo bharat-disk add          add a disk (guided)\n"
                                  "  bharat-disk list              the disks and what is on them\n"
                                  "  sudo bharat-disk forget NAME  stop opening /data/NAME at start-up\n"
                                  "  sudo bharat-disk swapfile 1G  extra swap space in a file\n\n"
                                  "More control: bdisk (parts), mkfs.ext4, mkfs.fat, mount, /etc/fstab\n"), 0)
                         : list();

    if (getuid() != 0) {

        fprintf(stderr, "bharat-disk: this needs root. Use: sudo bharat-disk %s\n", argv[1]);
        return 1;
    }

    if (!strcmp(argv[1], "add")) return add();
    if (!strcmp(argv[1], "forget") && argc == 3) return forget(argv[2]);
    if (!strcmp(argv[1], "swapfile") && argc == 3) return swapfile(argv[2]);

    fprintf(stderr, "bharat-disk: unknown command %s (try: bharat-disk)\n", argv[1]);
    return 1;
}
