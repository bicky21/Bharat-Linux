#ifndef LIBINSTALL_H
#define LIBINSTALL_H

/*
 * Installing Bharat Linux from the live system onto a disk.
 * Used by the graphical installer (greeter) and bharat-install (text).
 *
 * The disk gets a GPT table with two parts:
 *   1. BHARAT-EFI     256 MB  FAT32  the EFI file that starts the system
 *   2. BHARAT-SYSTEM  rest    ext4   the system, your files
 *
 * The EFI file is a "unified kernel image": the Linux kernel, a tiny
 * start-up file system (bootstrap) and the kernel command line in one
 * file, EFI/BOOT/BOOTX64.EFI, which every UEFI computer starts by itself.
 * No boot loader to configure. The installer writes the ID of the
 * system part into its command line.
 */

#include <ctype.h>
#include <dirent.h>
#include <utime.h>
#include <sched.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>

#include "libdisk.h"
#include "libuser.h"
#include "libaccount.h"

#define INSTALL_DIR    "/usr/share/bharat/install"
#define INSTALL_EFI    INSTALL_DIR "/bharat.efi"
#define INSTALL_TARGET "/run/install/target"
#define EFI_MB         256
#define MIN_DISK_MB    2048

/* the command line inside bharat.efi has this, replaced by the real ID */
#define ROOT_PLACEHOLDER "root=PARTUUID=00000000-0000-0000-0000-000000000000"

/* is this a live system (started from the USB stick or DVD)? */
static inline int is_live_system(void) {

    return access("/etc/live", F_OK) == 0;
}

/* the disk the live system was started from: not offered for installing */
static inline int is_live_medium(const char *disk) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128);

    for (int i = 0; i < n; i++) {

        if (strcmp(list[i].name, disk) && strcmp(list[i].parent, disk))
            continue;

        char dev[64];
        struct fs_info fs;

        snprintf(dev, sizeof(dev), "/dev/%.31s", list[i].name);

        if (probe_fs(dev, &fs) == 0 && !strcmp(fs.type, "iso9660") && !strncmp(fs.label, "BHARAT", 6))
            return 1;
    }

    return 0;
}

struct install_disk {
    char name[32];
    char description[160];     /* "sda  20 GB  QEMU HARDDISK" */
    long long bytes;
};

/* disks Bharat Linux can be installed on */
static inline int install_disks(struct install_disk *out, int max) {

    struct blockdev list[128];
    int n = list_blockdevs(list, 128), count = 0;

    for (int i = 0; i < n && count < max; i++) {

        struct blockdev *b = &list[i];

        if (b->partno || b->readonly || !strncmp(b->name, "sr", 2) ||
            b->bytes < (long long)MIN_DISK_MB * 1024 * 1024 ||
            disk_in_use(b->name) || is_live_medium(b->name))
            continue;

        char size[32];

        disk_size_text(b->bytes, size, sizeof(size));
        snprintf(out[count].name, sizeof(out[count].name), "%.31s", b->name);
        snprintf(out[count].description, sizeof(out[count].description), "%.31s   %s   %.60s%s",
                 b->name, size, b->model[0] ? b->model : "disk", b->removable ? "  (USB)" : "");
        out[count].bytes = b->bytes;
        count++;
    }

    return count;
}

/* reports progress: what is happening, and how far (0 to 100) */
typedef void (*install_progress)(const char *step, int percent);

/* ---------- copying the system ---------- */

/* folders that are not copied (they are made empty) */
static const char *install_skip[] = {
    "/proc", "/sys", "/dev", "/run", "/tmp", "/media", "/mnt", "/data", "/home",
    "/lost+found", "/var/log", "/var/lib/bharat/disk-backup", INSTALL_DIR, NULL
};

/* files that are not copied */
static const char *install_skip_files[] = {
    "/etc/live", "/etc/sudo-nopasswd", "/var/lib/bharat/last-user", "/swapfile", NULL
};

struct copy_state {
    long long done, total;     /* bytes */
    install_progress progress;
    int failed;
    char error[160];
};

static inline int install_skipped(const char *path, const char **list) {

    for (int i = 0; list[i]; i++)
        if (!strcmp(path, list[i]))
            return 1;

    return 0;
}

static inline long long tree_size(const char *dir) {

    DIR *d = opendir(dir);
    struct dirent *e;
    long long total = 0;

    while (d && (e = readdir(d)) != NULL) {

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char path[PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "%.3800s/%.255s", strcmp(dir, "/") ? dir : "", e->d_name);

        if (install_skipped(path, install_skip) || lstat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
            total += tree_size(path);
        else if (S_ISREG(st.st_mode))
            total += st.st_size;
    }

    if (d)
        closedir(d);

    return total;
}

static inline void copy_owner_and_times(const char *to, const struct stat *st) {

    if (lchown(to, st->st_uid, st->st_gid) != 0) {
        /* reported nowhere: the file is still usable */
    }

    if (!S_ISLNK(st->st_mode)) {

        chmod(to, st->st_mode & 07777);

        struct timespec t[2] = {st->st_atim, st->st_mtim};

        utimensat(AT_FDCWD, to, t, 0);
    }
}

static inline void copy_tree(const char *from, const char *to, struct copy_state *cs) {

    DIR *d = opendir(from);
    struct dirent *e;

    while (d && !cs->failed && (e = readdir(d)) != NULL) {

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char src[PATH_MAX], dst[PATH_MAX];
        struct stat st;

        snprintf(src, sizeof(src), "%.3800s/%.255s", strcmp(from, "/") ? from : "", e->d_name);
        snprintf(dst, sizeof(dst), "%.3800s/%.255s", to, e->d_name);

        if (lstat(src, &st) != 0 || install_skipped(src, install_skip_files))
            continue;

        if (install_skipped(src, install_skip)) {

            /* an empty folder, with the same rights (/tmp: 1777) */
            mkdir(dst, st.st_mode & 07777);
            copy_owner_and_times(dst, &st);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {

            mkdir(dst, 0700);
            copy_tree(src, dst, cs);
            copy_owner_and_times(dst, &st);
        }
        else if (S_ISLNK(st.st_mode)) {

            char target[PATH_MAX];
            ssize_t n = readlink(src, target, sizeof(target) - 1);

            if (n >= 0) {

                target[n] = 0;

                if (symlink(target, dst) == 0)
                    copy_owner_and_times(dst, &st);
            }
        }
        else if (S_ISREG(st.st_mode)) {

            int in = open(src, O_RDONLY | O_CLOEXEC);
            int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            static char buf[1 << 16];
            ssize_t r = 0;

            while (in >= 0 && out >= 0 && (r = read(in, buf, sizeof(buf))) > 0) {

                if (write(out, buf, (size_t)r) != r) {

                    r = -1;
                    break;
                }

                cs->done += r;
            }

            if (in < 0 || out < 0 || r < 0) {

                cs->failed = 1;
                snprintf(cs->error, sizeof(cs->error), "Copying %.100s failed: %s", src, strerror(errno));
            }

            if (in >= 0)
                close(in);

            if (out >= 0)
                close(out);

            copy_owner_and_times(dst, &st);

            /* tell how far we are, about every 2 MB */
            static long long last;

            if (cs->progress && cs->total && cs->done - last > 2 * 1024 * 1024) {

                last = cs->done;
                cs->progress("Copying the system", 30 + (int)(55 * cs->done / cs->total));
            }
        }
        /* device files and sockets are made by the running system */
    }

    if (d)
        closedir(d);
}

/* ---------- small helpers ---------- */

static inline int install_run(char *const argv[]) {

    int quiet = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);

    dup2(quiet, STDOUT_FILENO);
    dup2(quiet, STDERR_FILENO);

    int r = run_program(argv);

    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    close(quiet);

    return r;
}

static inline int copy_file(const char *from, const char *to, mode_t mode) {

    int in = open(from, O_RDONLY | O_CLOEXEC);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    static char buf[1 << 16];
    ssize_t r = 0;
    int ok = in >= 0 && out >= 0;

    while (ok && (r = read(in, buf, sizeof(buf))) > 0)
        ok = write(out, buf, (size_t)r) == r;

    ok = ok && r == 0 && fsync(out) == 0;

    if (in >= 0) close(in);
    if (out >= 0) close(out);

    return ok ? 0 : -1;
}

static inline void part_name(const char *disk, int nr, char *out, size_t size) {

    snprintf(out, size, "/dev/%.40s%s%d", disk, isdigit((unsigned char)disk[strlen(disk) - 1]) ? "p" : "", nr);
}

/* the EFI file, with the system part's ID put into its command line */
static inline int write_efi_file(const char *to, const char *partuuid) {

    int in = open(INSTALL_EFI, O_RDONLY | O_CLOEXEC);
    struct stat st;

    if (in < 0 || fstat(in, &st) != 0) {

        if (in >= 0)
            close(in);

        return -1;
    }

    char *data = malloc((size_t)st.st_size);
    int ok = data && read(in, data, (size_t)st.st_size) == st.st_size;

    close(in);

    char *at = NULL;
    size_t len = strlen(ROOT_PLACEHOLDER);

    for (off_t i = 0; ok && i + (off_t)len <= st.st_size; i++) {

        if (!memcmp(data + i, ROOT_PLACEHOLDER, len)) {

            at = data + i;
            break;
        }
    }

    if (!at) {

        free(data);
        return -1;
    }

    /* same length: "root=PARTUUID=" + 36 characters */
    char real[80];

    snprintf(real, sizeof(real), "root=PARTUUID=%s", partuuid);
    memcpy(at, real, len);

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    ok = out >= 0 && write(out, data, (size_t)st.st_size) == st.st_size && fsync(out) == 0;

    if (out >= 0)
        close(out);

    free(data);

    return ok ? 0 : -1;
}

/* ---------- the computer's boot menu (UEFI variables) ---------- */

#define EFIVARS "/sys/firmware/efi/efivars"
#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"

/* was this live system started in UEFI mode? */
static inline int started_in_uefi_mode(void) {

    return access("/sys/firmware/efi", F_OK) == 0;
}

/* efivarfs files are protected ("immutable") until this is cleared */
static inline void efivar_unprotect(const char *path) {

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int flags;

    if (fd < 0)
        return;

    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0) {

        flags &= ~FS_IMMUTABLE_FL;
        ioctl(fd, FS_IOC_SETFLAGS, &flags);
    }

    close(fd);
}

static inline int efivar_write(const char *name, const uint8_t *data, size_t len) {

    char path[256];
    uint8_t buf[1024];

    if (len + 4 > sizeof(buf))
        return -1;

    snprintf(path, sizeof(path), EFIVARS "/%s-" EFI_GLOBAL, name);
    efivar_unprotect(path);

    put32(buf, 0x7);    /* kept after power off, seen by the firmware and Linux */
    memcpy(buf + 4, data, len);

    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    int ok = fd >= 0 && write(fd, buf, len + 4) == (ssize_t)(len + 4);

    if (fd >= 0)
        close(fd);

    return ok ? 0 : -1;
}

/*
 * Adds "Bharat Linux" to the computer's own boot menu and puts it first
 * (what efibootmgr does on other systems). Without it, most computers
 * still start EFI/BOOT/BOOTX64.EFI by themselves; with it, they do so
 * even when another system is also installed.
 */
static inline int add_boot_entry(const struct gpt *t, int part, const char *file) {

    if (!started_in_uefi_mode())
        return -1;

    mount("efivarfs", EFIVARS, "efivarfs", 0, NULL);

    /* a free BootXXXX number */
    int number = -1;

    for (int n = 0x10; n < 0x100 && number < 0; n++) {

        char path[256];

        snprintf(path, sizeof(path), EFIVARS "/Boot%04X-" EFI_GLOBAL, n);

        if (access(path, F_OK) != 0)
            number = n;
    }

    if (number < 0)
        return -1;

    uint8_t opt[512];
    size_t k = 6;
    const char *desc = "Bharat Linux";

    put32(opt, 1);                       /* active */

    for (const char *c = desc; ; c++) {

        put16(opt + k, (uint16_t)*c);
        k += 2;

        if (!*c)
            break;
    }

    size_t path_start = k;
    const struct gpt_part *q = &t->part[part - 1];

    /* the part: hard drive media device path */
    opt[k] = 4; opt[k + 1] = 1; put16(opt + k + 2, 42);
    put32(opt + k + 4, (uint32_t)part);
    put64(opt + k + 8, q->first);
    put64(opt + k + 16, q->last - q->first + 1);
    guid_parse(q->uuid, opt + k + 24);
    opt[k + 40] = 2;                     /* GPT */
    opt[k + 41] = 2;                     /* signature is a GUID */
    k += 42;

    /* the file on it */
    size_t start = k;

    opt[k] = 4; opt[k + 1] = 4;
    k += 4;

    for (const char *c = file; ; c++) {

        put16(opt + k, (uint16_t)(*c == '/' ? '\\' : *c));
        k += 2;

        if (!*c)
            break;
    }

    put16(opt + start + 2, (uint16_t)(k - start));

    /* end of the path */
    opt[k] = 0x7F; opt[k + 1] = 0xFF; put16(opt + k + 2, 4);
    k += 4;

    put16(opt + 4, (uint16_t)(k - path_start));

    char name[16];

    snprintf(name, sizeof(name), "Boot%04X", number);

    if (efivar_write(name, opt, k) != 0)
        return -1;

    /* BootOrder: ours first, then the others as they were */
    uint8_t order[512];
    size_t len = 0;
    char path[256];

    snprintf(path, sizeof(path), EFIVARS "/BootOrder-" EFI_GLOBAL);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {

        uint8_t old[512];
        ssize_t n = read(fd, old, sizeof(old));

        close(fd);

        for (ssize_t i = 4; i + 1 < n && len + 4 < sizeof(order); i += 2) {

            if (le16(old + i) == number)
                continue;

            memcpy(order + 2 + len, old + i, 2);
            len += 2;
        }
    }

    put16(order, (uint16_t)number);

    return efivar_write("BootOrder", order, len + 2);
}

/* ---------- the whole installation ---------- */

/*
 * Installs onto `disk` (like "sda"). The account and computer name must
 * already be checked. Returns NULL when it worked, else what went wrong.
 * `error` holds the details.
 */
static inline const char *install_system(const char *disk, const char *user, const char *password,
                                         const char *host, install_progress progress,
                                         char *error, size_t error_size) {

    char efi_dev[64], sys_dev[64], path[PATH_MAX];
    struct gpt t;

    error[0] = 0;

    if (access(INSTALL_EFI, R_OK) != 0)
        return "This system was not started from the Bharat Linux installer (USB stick or DVD).";

    if (disk_in_use(disk) || is_live_medium(disk))
        return "That disk is in use.";

    /* 1. the table */
    progress("Preparing the disk", 2);

    snprintf(path, sizeof(path), "/dev/%.40s", disk);

    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return "The disk cannot be opened.";

    /* wipe old signatures at the start and end, so nothing old is found */
    static char zero[1 << 20];
    uint64_t bytes = disk_bytes(fd);

    if (pwrite(fd, zero, sizeof(zero), 0) != (ssize_t)sizeof(zero) ||
        pwrite(fd, zero, sizeof(zero), (off_t)(bytes - sizeof(zero))) != (ssize_t)sizeof(zero)) {

        close(fd);
        return "Writing to the disk failed.";
    }

    if (gpt_new(fd, &t) != 0 ||
        gpt_add(&t, (uint64_t)EFI_MB * 1024 * 1024, GPT_EFI, "BHARAT-EFI") != 1 ||
        gpt_add(&t, 0, GPT_LINUX, "BHARAT-SYSTEM") != 2 ||
        gpt_write(fd, &t) != 0) {

        close(fd);
        return "Making the parts on the disk failed.";
    }

    close(fd);

    char partuuid[40];

    snprintf(partuuid, sizeof(partuuid), "%s", t.part[1].uuid);

    for (char *c = partuuid; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    part_name(disk, 1, efi_dev, sizeof(efi_dev));
    part_name(disk, 2, sys_dev, sizeof(sys_dev));

    for (int i = 0; i < 100 && (access(efi_dev, F_OK) || access(sys_dev, F_OK)); i++)
        usleep(100 * 1000);

    if (access(sys_dev, F_OK) != 0)
        return "The new parts did not appear.";

    /* 2. file systems */
    progress("Formatting", 8);

    char *fat[] = {"/bin/mkfs.fat", "-F", "32", "-n", "BHARAT-EFI", efi_dev, NULL};
    char *ext4[] = {"/bin/mkfs.ext4", "-q", "-F", "-L", "bharat", sys_dev, NULL};

    if (install_run(fat) != 0 || install_run(ext4) != 0)
        return "Formatting failed.";

    /* 3. open them */
    progress("Opening the new system", 25);

    make_dirs(INSTALL_TARGET);
    umount2(INSTALL_TARGET "/boot/efi", MNT_DETACH);
    umount2(INSTALL_TARGET, MNT_DETACH);

    if (mount(sys_dev, INSTALL_TARGET, "ext4", MS_NOATIME, NULL) != 0)
        return "The new system part cannot be opened.";

    make_dirs(INSTALL_TARGET "/boot/efi");

    if (mount(efi_dev, INSTALL_TARGET "/boot/efi", "vfat", 0, "umask=0077") != 0) {

        umount(INSTALL_TARGET);
        return "The new EFI part cannot be opened.";
    }

    /* 4. copy */
    struct copy_state cs = {0, tree_size("/"), progress, 0, ""};

    progress("Copying the system", 30);
    copy_tree("/", INSTALL_TARGET, &cs);

    const char *err = NULL;

    if (cs.failed) {

        snprintf(error, error_size, "%s", cs.error);
        err = "Copying the system failed (is the disk too small or faulty?).";
    }

    /* 5. a fresh /etc: no live user, no password from the live system */
    progress("Setting up your account", 86);

    const char *fresh[] = {"passwd", "group", "hostname"};

    for (int i = 0; !err && i < 3; i++) {

        char from[256], to[256];

        snprintf(from, sizeof(from), INSTALL_DIR "/etc/%s", fresh[i]);
        snprintf(to, sizeof(to), INSTALL_TARGET "/etc/%s", fresh[i]);

        if (copy_file(from, to, 0644) != 0)
            err = "Writing the settings failed.";
    }

    snprintf(path, sizeof(path), INSTALL_TARGET "/etc/shadow");

    if (!err && copy_file(INSTALL_DIR "/etc/shadow", path, 0600) != 0)
        err = "Writing the settings failed.";

    /* the account is made inside the new system (chroot) */
    if (!err) {

        mount("devtmpfs", INSTALL_TARGET "/dev", "devtmpfs", 0, NULL);

        int pipefd[2];

        if (pipe(pipefd) != 0)
            err = "Creating the account failed.";

        pid_t pid = err ? -1 : fork();

        if (pid == 0) {

            close(pipefd[0]);

            const char *e = "The new system cannot be entered.";

            /* its own computer name space: the live system keeps its name */
            unshare(CLONE_NEWUTS);

            if (chroot(INSTALL_TARGET) == 0 && chdir("/") == 0)
                e = setup_first_account(user, password, password, host);

            if (e && write(pipefd[1], e, strlen(e)) < 0) {
                /* the parent sees an empty answer */
            }

            _exit(e ? 1 : 0);
        }

        if (pid > 0) {

            int st;
            char msg[160] = "";

            close(pipefd[1]);

            ssize_t n = read(pipefd[0], msg, sizeof(msg) - 1);

            close(pipefd[0]);
            waitpid(pid, &st, 0);

            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {

                msg[n > 0 ? n : 0] = 0;
                snprintf(error, error_size, "%s", msg);
                err = "Creating the account failed.";
            }
        }

        umount2(INSTALL_TARGET "/dev", MNT_DETACH);
    }

    /* 6. fstab: the EFI part opens at /boot/efi */
    if (!err) {

        char line[160];

        snprintf(line, sizeof(line), "PARTUUID=%s /boot/efi vfat umask=0077,nofail 0 2", t.part[0].uuid);
        snprintf(path, sizeof(path), INSTALL_TARGET "/etc/fstab");

        char from[] = INSTALL_DIR "/etc/fstab";

        if (copy_file(from, path, 0644) != 0 || rewrite_file(path, 0644, NULL, NULL, line) != 0)
            err = "Writing /etc/fstab failed.";
    }

    /* 7. the EFI file that starts the system */
    if (!err) {

        progress("Making it start by itself", 92);

        make_dirs(INSTALL_TARGET "/boot/efi/EFI/BOOT");
        make_dirs(INSTALL_TARGET "/boot/efi/EFI/bharat");

        if (write_efi_file(INSTALL_TARGET "/boot/efi/EFI/BOOT/BOOTX64.EFI", partuuid) != 0 ||
            write_efi_file(INSTALL_TARGET "/boot/efi/EFI/bharat/bharat.efi", partuuid) != 0)
            err = "Writing the start-up file failed.";

        /* not fatal: EFI/BOOT/BOOTX64.EFI is found without it */
        if (!err)
            add_boot_entry(&t, 1, "/EFI/bharat/bharat.efi");
    }

    /* 8. done: write everything to the disk */
    progress("Finishing", 97);
    sync();
    umount(INSTALL_TARGET "/boot/efi");

    if (umount(INSTALL_TARGET) != 0)
        umount2(INSTALL_TARGET, MNT_DETACH);

    sync();

    if (!err)
        progress("Done", 100);

    return err;
}

#endif
