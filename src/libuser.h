#ifndef LIBUSER_H
#define LIBUSER_H

/*
 * Bharat-linux user database.
 *
 * /etc/passwd   name:x:uid:gid:home:shell      (everyone may read it)
 * /etc/shadow   name:password-hash             (only root may read it)
 *
 * Passwords are never stored as text. Only a one-way SHA-512 hash is
 * stored, so reading the file does not reveal the password.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
/* glibc keeps crypt() in its own library with a header; musl has it built in */
#if __has_include(<crypt.h>)
#include <crypt.h>
#else
char *crypt(const char *key, const char *salt);
#endif
#include <sys/stat.h>
#include <sys/types.h>

struct bharat_user {
    char name[64];
    uid_t uid;
    gid_t gid;
    char home[128];
    char shell[128];
};

/* Reads one passwd line into u. Returns 1 if the line is valid. */
static inline int parse_passwd_line(char *line,
                                    struct bharat_user *u) {

    char *field[6];
    int count = 0;

    line[strcspn(line, "\r\n")] = 0;

    char *p = line;

    while (count < 6) {

        field[count++] = p;

        p = strchr(p, ':');

        if (!p)
            break;

        *p++ = 0;
    }

    if (count != 6 || field[0][0] == 0)
        return 0;

    snprintf(u->name, sizeof(u->name), "%s", field[0]);
    u->uid = (uid_t)atoi(field[2]);
    u->gid = (gid_t)atoi(field[3]);
    snprintf(u->home, sizeof(u->home), "%s", field[4]);
    snprintf(u->shell, sizeof(u->shell), "%s", field[5]);

    return 1;
}

/* Find a user by name (or by uid when name is NULL). Returns 1 if found. */
static inline int find_user(const char *name,
                            uid_t uid,
                            struct bharat_user *u) {

    FILE *f = fopen("/etc/passwd", "r");

    if (!f)
        return 0;

    char line[512];

    while (fgets(line, sizeof(line), f)) {

        if (!parse_passwd_line(line, u))
            continue;

        if (name ? strcmp(u->name, name) == 0 : u->uid == uid) {

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

/* The name of the user running this program ("unknown" if not listed). */
static inline const char *current_user_name(void) {

    static struct bharat_user u;

    if (find_user(NULL, getuid(), &u))
        return u.name;

    return "unknown";
}

/* Copies the stored hash of a user into hash. Needs root. */
static inline int read_hash(const char *name,
                            char *hash,
                            size_t size) {

    FILE *f = fopen("/etc/shadow", "r");

    if (!f)
        return 0;

    char line[512];

    while (fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\r\n")] = 0;

        char *colon = strchr(line, ':');

        if (!colon)
            continue;

        *colon = 0;

        if (strcmp(line, name) == 0) {

            snprintf(hash, size, "%s", colon + 1);

            fclose(f);

            return 1;
        }
    }

    fclose(f);

    return 0;
}

/* Compare without stopping at the first difference (hides timing). */
static inline int same_text(const char *a,
                            const char *b) {

    size_t la = strlen(a);
    size_t lb = strlen(b);
    unsigned char diff = la != lb;

    for (size_t i = 0; i < la && i < lb; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);

    return diff == 0;
}

/* Returns 1 if password is the right one for this user. Needs root. */
static inline int check_password(const char *name,
                                 const char *password) {

    char hash[256];

    if (!read_hash(name, hash, sizeof(hash)))
        return 0;

    /* "!" or "*" means: this account has no valid password */
    if (hash[0] != '$')
        return 0;

    /* crypt() repeats the same recipe (salt) that is stored in the hash */
    char *result = crypt(password, hash);

    return result && same_text(result, hash);
}

/* Makes a new SHA-512 hash with a fresh random salt. Returns 1 if ok. */
static inline int make_hash(const char *password,
                            char *hash,
                            size_t size) {

    static const char letters[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";

    unsigned char random[16];
    char salt[32] = "$6$";

    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0)
        return 0;

    ssize_t got = read(fd, random, sizeof(random));

    close(fd);

    if (got != (ssize_t)sizeof(random))
        return 0;

    for (int i = 0; i < 16; i++)
        salt[3 + i] = letters[random[i] % 64];

    salt[19] = '$';
    salt[20] = 0;

    char *result = crypt(password, salt);

    if (!result || result[0] != '$')
        return 0;

    snprintf(hash, size, "%s", result);

    return 1;
}

/* Stores a new hash for a user in /etc/shadow. Needs root. */
static inline int write_hash(const char *name,
                             const char *hash) {

    FILE *in = fopen("/etc/shadow", "r");

    int fd = open("/etc/shadow.tmp",
                  O_WRONLY | O_CREAT | O_TRUNC,
                  0600);

    if (fd < 0) {

        if (in)
            fclose(in);

        return 0;
    }

    FILE *out = fdopen(fd, "w");

    char line[512];
    int replaced = 0;

    while (in && fgets(line, sizeof(line), in)) {

        size_t len = strlen(name);

        if (strncmp(line, name, len) == 0 && line[len] == ':') {

            fprintf(out, "%s:%s\n", name, hash);
            replaced = 1;
        }
        else {

            fputs(line, out);
        }
    }

    if (!replaced)
        fprintf(out, "%s:%s\n", name, hash);

    if (in)
        fclose(in);

    if (fclose(out) != 0)
        return 0;

    /* rename swaps the file in one step, so it is never half-written */
    return rename("/etc/shadow.tmp", "/etc/shadow") == 0;
}

/*
 * /etc/group   name:x:gid:member1,member2
 */

/* Group name -> gid. Returns 1 if found. */
static inline int find_group(const char *name,
                             gid_t *gid) {

    FILE *f = fopen("/etc/group", "r");

    if (!f)
        return 0;

    char line[512];
    int found = 0;

    while (!found && fgets(line, sizeof(line), f)) {

        char *colon = strchr(line, ':');

        if (!colon)
            continue;

        *colon = 0;

        char *second = strchr(colon + 1, ':');

        if (strcmp(line, name) == 0 && second) {

            *gid = (gid_t)atoi(second + 1);
            found = 1;
        }
    }

    fclose(f);

    return found;
}

/* gid -> group name ("" if unknown). */
static inline void group_name(gid_t gid,
                              char *out,
                              size_t size) {

    FILE *f = fopen("/etc/group", "r");

    out[0] = 0;

    if (!f)
        return;

    char line[512];

    while (fgets(line, sizeof(line), f)) {

        char *colon = strchr(line, ':');

        if (!colon)
            continue;

        *colon = 0;

        char *second = strchr(colon + 1, ':');

        if (second && (gid_t)atoi(second + 1) == gid) {

            snprintf(out, size, "%.63s", line);
            break;
        }
    }

    fclose(f);
}

/* The extra groups a user is a member of. Returns how many. */
static inline int user_groups(const char *user,
                              gid_t *list,
                              int max) {

    FILE *f = fopen("/etc/group", "r");

    if (!f)
        return 0;

    char line[512];
    int count = 0;

    while (count < max && fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\r\n")] = 0;

        /* split into the 4 fields */
        char *field[4] = {line, NULL, NULL, NULL};

        for (int i = 1; i < 4; i++) {

            char *colon = field[i - 1] ? strchr(field[i - 1], ':') : NULL;

            if (colon) {

                *colon = 0;
                field[i] = colon + 1;
            }
        }

        if (!field[3])
            continue;

        for (char *m = strtok(field[3], ","); m; m = strtok(NULL, ",")) {

            if (strcmp(m, user) == 0) {

                list[count++] = (gid_t)atoi(field[2]);
                break;
            }
        }
    }

    fclose(f);

    return count;
}

/* Sets TZ from /etc/timezone, written like "IST-5:30" (5:30 ahead of UTC). */
static inline void load_timezone(void) {

    FILE *tz = fopen("/etc/timezone", "r");
    char zone[64];

    if (!tz)
        return;

    if (fgets(zone, sizeof(zone), tz)) {

        zone[strcspn(zone, "\r\n")] = 0;
        setenv("TZ", zone, 1);
    }

    fclose(tz);
}

/*
 * Turns this (root) process into the given user, for good:
 * groups, ids, environment, home folder. Used by login and greeter.
 * Returns 0 if it worked.
 */
#include <grp.h>

static inline int become_user(const struct bharat_user *user) {

    /*
     * Drop root. The order matters: groups first, then gid, then uid.
     * The extra groups (from /etc/group) decide for example who may
     * use the screen (video) and the mouse (input).
     * After setuid() we can never get root back.
     */
    gid_t groups[32];
    int group_count = user_groups(user->name, groups, 32);

    if (setgroups(group_count, groups) != 0 ||
        setgid(user->gid) != 0 ||
        setuid(user->uid) != 0)
        return -1;

    clearenv();

    setenv("USER", user->name, 1);
    setenv("LOGNAME", user->name, 1);
    setenv("HOME", user->home, 1);
    setenv("SHELL", user->shell, 1);
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("TERM", "linux", 1);

    load_timezone();

    if (chdir(user->home) != 0 && chdir("/") != 0)
        return -1;

    return 0;
}

/* Reads a line from the keyboard without showing it. Returns 1 if ok. */
#include <termios.h>

static inline int read_hidden(const char *prompt,
                              char *out,
                              size_t size) {

    /*
     * Talk to the terminal itself (/dev/tty), not to stdin/stdout: in
     * "sudo dmesg | tail" the output goes into the pipe, but the
     * password prompt must still appear on the screen.
     */
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    int in = tty >= 0 ? tty : STDIN_FILENO;
    int outfd = tty >= 0 ? tty : STDOUT_FILENO;

    struct termios old, hidden;
    int has_terminal = tcgetattr(in, &old) == 0;

    /*
     * Hide the typing BEFORE showing the prompt. The other way round,
     * letters typed right after the prompt appears would still show.
     */
    if (has_terminal) {

        hidden = old;
        hidden.c_lflag &= ~ECHO;

        tcsetattr(in, TCSANOW, &hidden);
    }

    fflush(stdout);

    if (write(outfd, prompt, strlen(prompt)) < 0) {
        /* no screen to write to: still read the answer */
    }

    /* read one line, a byte at a time (no buffering past the line) */
    size_t n = 0;
    int ok = 0;
    char c;

    while (read(in, &c, 1) == 1) {

        ok = 1;

        if (c == '\n' || c == '\r')
            break;

        if (n + 1 < size)
            out[n++] = c;
    }

    out[n] = 0;

    if (has_terminal) {

        tcsetattr(in, TCSANOW, &old);

        if (write(outfd, "\n", 1) < 0) {
            /* nothing to do */
        }
    }

    if (tty >= 0)
        close(tty);

    return ok;
}

#endif
