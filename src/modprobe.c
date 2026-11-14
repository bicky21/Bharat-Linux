/*
 * modprobe <name>
 *
 * Loads a kernel module (driver) and the modules it needs first.
 * Module files live in /lib/modules/<kernel version>/ and are usually
 * compressed (.ko.zst); the kernel unpacks them itself.
 * modules.dep says which modules each module needs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

static char base[256];       /* /lib/modules/<version> */

/* "8139-too" and "8139_too" are the same module: compare with '_' */
static void normalize(const char *in, char *out, size_t size) {

    size_t i = 0;

    for (; in[i] && i + 1 < size; i++)
        out[i] = in[i] == '-' ? '_' : in[i];

    out[i] = 0;
}

/* "kernel/drivers/input/mouse/psmouse.ko.zst" -> "psmouse" */
static void module_name(const char *path, char *out, size_t size) {

    const char *slash = strrchr(path, '/');
    char tmp[256];

    snprintf(tmp, sizeof(tmp), "%s", slash ? slash + 1 : path);

    char *dot = strstr(tmp, ".ko");

    if (dot)
        *dot = 0;

    normalize(tmp, out, size);
}

static int is_loaded(const char *name) {

    char path[300];
    struct stat st;

    snprintf(path, sizeof(path), "/sys/module/%s/initstate", name);

    return stat(path, &st) == 0;
}

static int is_builtin(const char *name) {

    char path[300];

    snprintf(path, sizeof(path), "%s/modules.builtin", base);

    FILE *f = fopen(path, "r");

    if (!f)
        return 0;

    char line[512], n[256];
    int found = 0;

    while (!found && fgets(line, sizeof(line), f)) {

        line[strcspn(line, "\n")] = 0;
        module_name(line, n, sizeof(n));
        found = strcmp(n, name) == 0;
    }

    fclose(f);

    return found;
}

static int load_file(const char *relative) {

    char name[256];
    char path[600];

    module_name(relative, name, sizeof(name));

    if (is_loaded(name))
        return 0;

    if (relative[0] == '/')
        snprintf(path, sizeof(path), "%s", relative);
    else
        snprintf(path, sizeof(path), "%s/%s", base, relative);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {

        fprintf(stderr, "modprobe: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    int compressed = strstr(path, ".ko.") != NULL;

    long r = syscall(SYS_finit_module, fd, "",
                     compressed ? MODULE_INIT_COMPRESSED_FILE : 0);

    int err = errno;

    close(fd);

    if (r != 0 && err != EEXIST) {

        fprintf(stderr, "modprobe: %s: %s\n", name, strerror(err));
        return 1;
    }

    return 0;
}

/* Search the module folder when there is no modules.dep. */
static int find_file(const char *dir, const char *want, char *out, size_t size) {

    DIR *d = opendir(dir);

    if (!d)
        return 0;

    struct dirent *e;
    int found = 0;

    while (!found && (e = readdir(d)) != NULL) {

        if (e->d_name[0] == '.')
            continue;

        char path[1024];

        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        struct stat st;

        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {

            found = find_file(path, want, out, size);
        }
        else if (strstr(e->d_name, ".ko")) {

            char n[256];

            module_name(e->d_name, n, sizeof(n));

            if (strcmp(n, want) == 0) {

                snprintf(out, size, "%s", path);
                found = 1;
            }
        }
    }

    closedir(d);

    return found;
}

static int load(const char *wanted) {

    char name[256];

    normalize(wanted, name, sizeof(name));

    if (is_loaded(name) || is_builtin(name))
        return 0;

    char path[300];

    snprintf(path, sizeof(path), "%s/modules.dep", base);

    FILE *f = fopen(path, "r");

    if (f) {

        static char line[8192];

        while (fgets(line, sizeof(line), f)) {

            line[strcspn(line, "\n")] = 0;

            char *colon = strchr(line, ':');

            if (!colon)
                continue;

            *colon = 0;

            char n[256];

            module_name(line, n, sizeof(n));

            if (strcmp(n, name) != 0)
                continue;

            fclose(f);

            /*
             * The list after ':' holds everything this module needs.
             * The last one has no needs of its own, so load from the end.
             */
            char *deps[128];
            int count = 0;

            for (char *tok = strtok(colon + 1, " "); tok && count < 128; tok = strtok(NULL, " "))
                deps[count++] = tok;

            int failed = 0;

            for (int i = count - 1; i >= 0; i--)
                failed |= load_file(deps[i]);

            return failed | load_file(line);
        }

        fclose(f);
    }

    char found[1024];

    if (find_file(base, name, found, sizeof(found)))
        return load_file(found);

    fprintf(stderr, "modprobe: module %s not found in %s\n", wanted, base);

    return 1;
}

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: modprobe <module> [module...]\n");
        return 1;
    }

    struct utsname u;

    uname(&u);
    snprintf(base, sizeof(base), "/lib/modules/%.200s", u.release);

    int failed = 0;

    for (int i = 1; i < argc; i++)
        failed |= load(argv[i]);

    return failed;
}
