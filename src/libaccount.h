#ifndef LIBACCOUNT_H
#define LIBACCOUNT_H

/*
 * Bharat Linux account management (needs root).
 *
 * Used by the first-boot setup (greeter, login), useradd, userdel,
 * usermod, passwd and sudo. Every change to /etc/passwd, /etc/group and
 * /etc/shadow is written to a new file first and then renamed over the
 * old one, so a crash can never leave a half-written file behind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libuser.h"

#define FIRST_UID 1000
#define ADMIN_GROUP "wheel"

/* groups every new user joins: screen, mouse/keyboard, everyone */
static const char *default_groups[] = {"users", "video", "input"};

/* ---------- the security log ---------- */

/* Appends one line to /var/log/auth.log (readable by root only). */
static inline void auth_log(const char *fmt, ...) {

    int fd = open("/var/log/auth.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);

    if (fd < 0)
        return;

    char line[512];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    size_t n = strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S ", tm);

    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);

    size_t len = strlen(line);

    line[len++] = '\n';

    if (write(fd, line, len) < 0) {
        /* nothing more we can do */
    }

    close(fd);
}

/* ---------- checks ---------- */

/* Names like "bicky" or "ravi_k": small letters, digits, _ and -. */
static inline int valid_username(const char *name) {

    size_t len = strlen(name);

    if (len == 0 || len > 31)
        return 0;

    if (!(name[0] >= 'a' && name[0] <= 'z') && name[0] != '_')
        return 0;

    for (size_t i = 1; i < len; i++) {

        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }

    return 1;
}

/* NULL if the password is good enough, otherwise the reason. */
static inline const char *weak_password(const char *password, const char *user) {

    if (strlen(password) < 8)
        return "The password needs at least 8 characters.";

    if (user && strcmp(password, user) == 0)
        return "The password must not be the user name.";

    return NULL;
}

static inline int group_exists(const char *name) {

    gid_t gid;

    return find_group(name, &gid);
}

/* How many normal (not system) users exist. 0 means: first boot. */
static inline int normal_user_count(void) {

    FILE *f = fopen("/etc/passwd", "r");
    char line[512];
    struct bharat_user u;
    int count = 0;

    while (f && fgets(line, sizeof(line), f)) {

        if (parse_passwd_line(line, &u) && u.uid >= FIRST_UID && u.uid < 60000)
            count++;
    }

    if (f)
        fclose(f);

    return count;
}

/* The lowest free number at or above 1000, for both uid and gid. */
static inline unsigned next_free_id(void) {

    unsigned id = FIRST_UID;
    char line[512];

    for (int pass = 0; pass < 2; pass++) {

        FILE *f = fopen(pass == 0 ? "/etc/passwd" : "/etc/group", "r");

        while (f && fgets(line, sizeof(line), f)) {

            /* the number is the 3rd field in both files */
            char *p = strchr(line, ':');

            p = p ? strchr(p + 1, ':') : NULL;

            if (p) {

                unsigned n = (unsigned)atoi(p + 1);

                if (n >= id && n < 60000)
                    id = n + 1;
            }
        }

        if (f)
            fclose(f);
    }

    return id;
}

/* ---------- safe file editing ---------- */

/*
 * Rewrites a file line by line. edit() gets each line (without '\n')
 * and returns: 1 keep it (it may change it in place), 0 drop it.
 * extra (may be NULL) is added at the end. Returns 0 if it worked.
 */
static inline int rewrite_file(const char *path, mode_t mode,
                               int (*edit)(char *line, void *ctx), void *ctx,
                               const char *extra) {

    char tmp[4200];

    snprintf(tmp, sizeof(tmp), "%s.new", path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);

    if (fd < 0)
        return -1;

    FILE *out = fdopen(fd, "w");
    FILE *in = fopen(path, "r");
    char line[1024];

    while (in && fgets(line, sizeof(line), in)) {

        line[strcspn(line, "\n")] = 0;

        if (!edit || edit(line, ctx))
            fprintf(out, "%s\n", line);
    }

    if (in)
        fclose(in);

    if (extra)
        fprintf(out, "%s\n", extra);

    /* make sure the new file is really on the disk before swapping */
    fflush(out);
    fsync(fd);

    if (fclose(out) != 0) {

        unlink(tmp);
        return -1;
    }

    chmod(tmp, mode);

    return rename(tmp, path);
}

/* drop lines that start with "name:" */
static int drop_name(char *line, void *ctx) {

    const char *name = ctx;
    size_t n = strlen(name);

    return !(strncmp(line, name, n) == 0 && line[n] == ':');
}

struct member_edit {
    const char *group;
    const char *user;
    int add;          /* 1 add, 0 remove; group NULL = remove from all */
};

/* add or remove a user in the 4th field ("a,b,c") of a group line */
static int edit_member(char *line, void *ctx) {

    struct member_edit *m = ctx;
    char copy[1024];

    snprintf(copy, sizeof(copy), "%s", line);

    char *field[4] = {copy, NULL, NULL, NULL};

    for (int i = 1; i < 4; i++) {

        char *colon = field[i - 1] ? strchr(field[i - 1], ':') : NULL;

        if (colon) {

            *colon = 0;
            field[i] = colon + 1;
        }
    }

    if (!field[3] || (m->group && strcmp(field[0], m->group) != 0))
        return 1;

    char members[768] = "";
    int present = 0;

    for (char *u = strtok(field[3], ","); u; u = strtok(NULL, ",")) {

        if (strcmp(u, m->user) == 0) {

            present = 1;

            if (!m->add)
                continue;
        }

        if (members[0])
            strncat(members, ",", sizeof(members) - strlen(members) - 1);

        strncat(members, u, sizeof(members) - strlen(members) - 1);
    }

    if (m->add && !present) {

        if (members[0])
            strncat(members, ",", sizeof(members) - strlen(members) - 1);

        strncat(members, m->user, sizeof(members) - strlen(members) - 1);
    }

    /* build the new line, then copy it back (lines are at most 1023 long) */
    char rebuilt[2048];
    int n = snprintf(rebuilt, sizeof(rebuilt), "%s:%s:%s:%s", field[0], field[1], field[2], members);

    if (n < 0 || n >= 1024)
        return 1;          /* too long to be real: leave the line as it was */

    memcpy(line, rebuilt, (size_t)n + 1);

    return 1;
}

static inline int set_group_member(const char *group, const char *user, int add) {

    struct member_edit m = {group, user, add};

    return rewrite_file("/etc/group", 0644, edit_member, &m, NULL);
}

static inline int is_group_member(const char *group, const char *user) {

    gid_t list[64], gid;
    int n = user_groups(user, list, 64);

    if (!find_group(group, &gid))
        return 0;

    for (int i = 0; i < n; i++) {

        if (list[i] == gid)
            return 1;
    }

    return 0;
}

static inline int is_admin(const char *user) {

    return strcmp(user, "root") == 0 || is_group_member(ADMIN_GROUP, user);
}

/* ---------- locking ---------- */

/* A locked account has "!" in front of its hash: no password works. */
static inline int set_locked(const char *name, int lock) {

    char hash[256], updated[260];

    if (!read_hash(name, hash, sizeof(hash)))
        return -1;

    if (lock && hash[0] != '!')
        snprintf(updated, sizeof(updated), "!%s", hash);
    else if (!lock && hash[0] == '!')
        snprintf(updated, sizeof(updated), "%s", hash + 1);
    else
        return 0;

    if (!lock && updated[0] == 0)
        return -2;          /* never had a password: set one with passwd */

    return write_hash(name, updated) ? 0 : -1;
}

/* ---------- creating and deleting users ---------- */

/* Copies the files of /etc/skel (start-up settings) into a new home. */
static inline void copy_skel(const char *home, uid_t uid, gid_t gid) {

    DIR *d = opendir("/etc/skel");
    struct dirent *e;

    while (d && (e = readdir(d)) != NULL) {

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char from[300], to[600];

        snprintf(from, sizeof(from), "/etc/skel/%.255s", e->d_name);
        snprintf(to, sizeof(to), "%.300s/%.255s", home, e->d_name);

        int in = open(from, O_RDONLY | O_CLOEXEC);
        int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

        if (in >= 0 && out >= 0) {

            char buf[4096];
            ssize_t n;

            while ((n = read(in, buf, sizeof(buf))) > 0) {

                if (write(out, buf, n) != n)
                    break;
            }

            if (fchown(out, uid, gid) != 0) {
                /* the file stays root's; still readable */
            }
        }

        if (in >= 0) close(in);
        if (out >= 0) close(out);
    }

    if (d)
        closedir(d);
}

/*
 * Creates a user: an entry in passwd, a private group with the same
 * name, a locked password (set it with passwd), the default groups,
 * and a home folder. admin = also in the wheel group (may use sudo).
 * Returns NULL if it worked, otherwise what went wrong.
 */
static inline const char *create_user(const char *name, int admin) {

    struct bharat_user existing;

    if (!valid_username(name))
        return "Use small letters, digits, _ or - (starting with a letter).";

    if (find_user(name, 0, &existing) || group_exists(name))
        return "That name is already taken.";

    unsigned id = next_free_id();
    char line[512], home[300];

    snprintf(home, sizeof(home), "/home/%s", name);

    snprintf(line, sizeof(line), "%s:x:%u:%u:%s:/bin/sh", name, id, id, home);

    if (rewrite_file("/etc/passwd", 0644, NULL, NULL, line) != 0)
        return "Could not write /etc/passwd.";

    snprintf(line, sizeof(line), "%s:x:%u:", name, id);
    rewrite_file("/etc/group", 0644, NULL, NULL, line);

    /* no password yet: the account cannot be used until one is set */
    snprintf(line, sizeof(line), "%s:!", name);
    rewrite_file("/etc/shadow", 0600, NULL, NULL, line);

    for (size_t i = 0; i < sizeof(default_groups) / sizeof(*default_groups); i++)
        set_group_member(default_groups[i], name, 1);

    if (admin)
        set_group_member(ADMIN_GROUP, name, 1);

    mkdir("/home", 0755);

    if (mkdir(home, 0700) == 0 || errno == EEXIST) {

        if (chown(home, id, id) != 0 || chmod(home, 0700) != 0) {
            /* reported by the caller noticing the home is unusable */
        }

        copy_skel(home, id, id);
    }

    auth_log("account created: %s (uid %u)%s", name, id, admin ? ", administrator" : "");

    return NULL;
}

/* Deletes a folder and everything in it (for userdel -r). */
static inline void remove_tree(const char *path) {

    struct stat st;

    if (lstat(path, &st) != 0)
        return;

    if (S_ISDIR(st.st_mode)) {

        DIR *d = opendir(path);
        struct dirent *e;

        while (d && (e = readdir(d)) != NULL) {

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[1024];

            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            remove_tree(child);
        }

        if (d)
            closedir(d);

        rmdir(path);
    }
    else {

        unlink(path);
    }
}

static inline const char *delete_user(const char *name, int remove_home) {

    struct bharat_user u;

    if (!find_user(name, 0, &u))
        return "No such user.";

    if (u.uid == 0)
        return "The root account cannot be deleted.";

    rewrite_file("/etc/passwd", 0644, drop_name, (void *)name, NULL);
    rewrite_file("/etc/shadow", 0600, drop_name, (void *)name, NULL);
    rewrite_file("/etc/group", 0644, drop_name, (void *)name, NULL);

    /* and out of every group it was a member of */
    struct member_edit m = {NULL, name, 0};

    rewrite_file("/etc/group", 0644, edit_member, &m, NULL);

    if (remove_home && strncmp(u.home, "/home/", 6) == 0)
        remove_tree(u.home);

    auth_log("account deleted: %s%s", name, remove_home ? " (home removed)" : "");

    return NULL;
}

/* ---------- first start-up ---------- */

/*
 * The first boot has no normal user and a locked root: the person
 * installing creates their own account, which becomes an administrator.
 * No password ever ships inside the system image.
 */
static inline int first_boot(void) {

    return normal_user_count() == 0;
}

static inline const char *set_password(const char *name, const char *password) {

    char hash[256];
    const char *weak = weak_password(password, name);

    if (weak)
        return weak;

    if (!make_hash(password, hash, sizeof(hash)) || !write_hash(name, hash))
        return "Could not save the password.";

    return NULL;
}

static inline const char *set_computer_name(const char *host) {

    size_t len = strlen(host);

    if (len == 0 || len > 63)
        return "The computer name needs 1 to 63 characters.";

    for (size_t i = 0; i < len; i++) {

        char c = host[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return "The computer name may use letters, digits and -.";
    }

    FILE *f = fopen("/etc/hostname", "w");

    if (f) {

        fprintf(f, "%s\n", host);
        fclose(f);
    }

    if (sethostname(host, len) != 0) {
        /* saved in /etc/hostname; used from the next start */
    }

    return NULL;
}

/* Creates the first (administrator) account. NULL = success. */
static inline const char *setup_first_account(const char *name, const char *password,
                                              const char *repeat, const char *host) {

    if (strcmp(password, repeat) != 0)
        return "The two passwords are different.";

    const char *err = weak_password(password, name);

    if (!err)
        err = set_computer_name(host);

    if (!err)
        err = create_user(name, 1);

    if (!err)
        err = set_password(name, password);

    return err;
}

#endif
