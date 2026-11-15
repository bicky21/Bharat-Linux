/*
 * Commands about the system:
 * pwd uname hostname uptime date cal sleep env printenv which clear
 * free nproc dmesg mount umount sync killall pkill pidof lsblk lscpu
 * lsmod tty arch
 */

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include "coreutils.h"
#include "../libdisk.h"

extern char **environ;

COMMAND(pwd) {

    (void)argc;
    (void)argv;

    char cwd[PATH_MAX];

    /* $PWD keeps the path the user typed (with links), like Ubuntu */
    const char *pwd = getenv("PWD");
    struct stat a, b;

    if (pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && !(argc > 1 && strcmp(argv[1], "-P") == 0)) {

        printf("%s\n", pwd);
        return 0;
    }

    if (!getcwd(cwd, sizeof(cwd)))
        return fail("%s", strerror(errno));

    printf("%s\n", cwd);

    return 0;
}

COMMAND(uname) {

    int flags[128] = {0};
    char *values[128] = {0};
    options(argc, argv, "asnrvmpio", "", flags, values);

    struct utsname u;

    uname(&u);

    if (flags['a'])
        flags['s'] = flags['n'] = flags['r'] = flags['v'] = flags['m'] = flags['o'] = 1;

    if (!flags['s'] && !flags['n'] && !flags['r'] && !flags['v'] && !flags['m'] && !flags['o'] &&
        !flags['p'] && !flags['i'])
        flags['s'] = 1;

    const char *parts[8];
    int n = 0;

    if (flags['s']) parts[n++] = u.sysname;
    if (flags['n']) parts[n++] = u.nodename;
    if (flags['r']) parts[n++] = u.release;
    if (flags['v']) parts[n++] = u.version;
    if (flags['m'] || flags['p'] || flags['i']) parts[n++] = u.machine;
    if (flags['o']) parts[n++] = "Bharat Linux";

    for (int i = 0; i < n; i++)
        printf("%s%s", i ? " " : "", parts[i]);

    printf("\n");

    return 0;
}

COMMAND(arch) {

    (void)argc;
    (void)argv;

    struct utsname u;

    uname(&u);
    printf("%s\n", u.machine);

    return 0;
}

COMMAND(hostname) {

    if (argc < 2) {

        char host[256];

        gethostname(host, sizeof(host));
        printf("%s\n", host);

        return 0;
    }

    if (strcmp(argv[1], "-I") == 0 || strcmp(argv[1], "-i") == 0)
        return execlp("ipaddr", "ipaddr", (char *)NULL), fail("cannot run ipaddr");

    /* set a new name: for now and for the next start */
    if (sethostname(argv[1], strlen(argv[1])) != 0)
        return fail("cannot set the name: %s (try: sudo hostname %s)", strerror(errno), argv[1]);

    FILE *f = fopen("/etc/hostname", "w");

    if (f) {

        fprintf(f, "%s\n", argv[1]);
        fclose(f);
    }

    return 0;
}

static void format_uptime(long secs, char *out, size_t size) {

    long days = secs / 86400, hours = secs / 3600 % 24, mins = secs / 60 % 60;

    if (days)
        snprintf(out, size, "%ld day%s, %2ld:%02ld", days, days == 1 ? "" : "s", hours, mins);
    else if (hours)
        snprintf(out, size, "%2ld:%02ld", hours, mins);
    else
        snprintf(out, size, "%ld min", mins);
}

COMMAND(uptime) {

    struct sysinfo si;

    sysinfo(&si);

    if (argc > 1 && strcmp(argv[1], "-p") == 0) {

        long h = si.uptime / 3600, m = si.uptime / 60 % 60;

        printf("up %s%ld hour%s, %ld minute%s\n", "", h, h == 1 ? "" : "s", m, m == 1 ? "" : "s");
        return 0;
    }

    char now[32], up[64];
    time_t t = time(NULL);

    strftime(now, sizeof(now), "%H:%M:%S", localtime(&t));
    format_uptime(si.uptime, up, sizeof(up));

    double scale = 1 << SI_LOAD_SHIFT;

    printf(" %s up %s,  load average: %.2f, %.2f, %.2f\n", now, up,
           si.loads[0] / scale, si.loads[1] / scale, si.loads[2] / scale);

    return 0;
}

COMMAND(date) {

    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    int utc = 0;

    for (int i = 1; i < argc; i++) {

        if (argv[i][0] == '+')
            fmt = argv[i] + 1;
        else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--utc") == 0)
            utc = 1;
        else if (strcmp(argv[i], "-R") == 0)
            fmt = "%a, %d %b %Y %H:%M:%S %z";
        else if (strcmp(argv[i], "-I") == 0)
            fmt = "%Y-%m-%d";
        else
            return fail("usage: date [-u] [+FORMAT]   (example: date +%%H:%%M)");
    }

    time_t t = time(NULL);
    char out[512];

    strftime(out, sizeof(out), fmt, utc ? gmtime(&t) : localtime(&t));
    printf("%s\n", out);

    return 0;
}

/* cal [[MONTH] YEAR]: a calendar, today highlighted */
COMMAND(cal) {

    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    int month = today.tm_mon + 1, year = today.tm_year + 1900;

    if (argc == 2)
        year = atoi(argv[1]), month = 0;
    else if (argc == 3)
        month = atoi(argv[1]), year = atoi(argv[2]);

    if (year < 1 || year > 9999 || month < 0 || month > 12)
        return fail("usage: cal [[MONTH] YEAR]");

    static const char *names[] = {"January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December"};
    int first = month ? month : 1, last = month ? month : 12;
    int tty = isatty(STDOUT_FILENO);

    for (int m = first; m <= last; m++) {

        char title[64];

        snprintf(title, sizeof(title), "%s %d", names[m - 1], year);
        printf("%*s\n", (int)(10 + strlen(title) / 2), title);
        printf("Su Mo Tu We Th Fr Sa\n");

        struct tm t = {.tm_year = year - 1900, .tm_mon = m - 1, .tm_mday = 1, .tm_hour = 12};

        mktime(&t);

        int days = 31;

        if (m == 4 || m == 6 || m == 9 || m == 11) days = 30;
        if (m == 2) days = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;

        printf("%*s", t.tm_wday * 3, "");

        for (int d = 1; d <= days; d++) {

            int is_today = tty && d == today.tm_mday && m == today.tm_mon + 1 && year == today.tm_year + 1900;

            printf(is_today ? "\033[7m%2d\033[0m" : "%2d", d);

            if ((t.tm_wday + d) % 7 == 0)
                printf("\n");
            else if (d < days)
                printf(" ");
        }

        printf("\n%s", m < last ? "\n" : "");
    }

    return 0;
}

/* sleep 5, sleep 0.5, sleep 2m, sleep 1h */
COMMAND(sleep) {

    if (argc < 2)
        return fail("usage: sleep NUMBER[s|m|h|d]");

    double total = 0;

    for (int i = 1; i < argc; i++) {

        char *end;
        double v = strtod(argv[i], &end);

        if (*end == 'm') v *= 60;
        else if (*end == 'h') v *= 3600;
        else if (*end == 'd') v *= 86400;
        else if (*end && *end != 's') return fail("invalid time '%s'", argv[i]);

        total += v;
    }

    struct timespec ts = {(time_t)total, (long)((total - (time_t)total) * 1e9)};

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* keep sleeping for the time that is left */
    }

    return 0;
}

/* env: show the settings; env NAME=value command: run with extra settings */
COMMAND(env) {

    int i = 1;

    if (i < argc && strcmp(argv[i], "-i") == 0) {

        clearenv();
        i++;
    }

    for (; i < argc && strchr(argv[i], '='); i++)
        putenv(argv[i]);

    if (i < argc) {

        execvp(argv[i], &argv[i]);
        return fail("%s: %s", argv[i], strerror(errno)), 127;
    }

    for (char **e = environ; *e; e++)
        printf("%s\n", *e);

    return 0;
}

COMMAND(printenv) {

    if (argc < 2)
        return cmd_env(1, argv);

    int failed = 0;

    for (int i = 1; i < argc; i++) {

        const char *v = getenv(argv[i]);

        if (v)
            printf("%s\n", v);
        else
            failed = 1;
    }

    return failed;
}

COMMAND(which) {

    int all = argc > 1 && strcmp(argv[1], "-a") == 0;
    int failed = 0;
    const char *path = getenv("PATH");

    if (!path)
        path = "/bin:/usr/bin";

    for (int i = all ? 2 : 1; i < argc; i++) {

        int found = 0;

        if (strchr(argv[i], '/')) {

            if (access(argv[i], X_OK) == 0)
                printf("%s\n", argv[i]), found = 1;
        }
        else {

            char copy[4096];

            snprintf(copy, sizeof(copy), "%s", path);

            for (char *dir = strtok(copy, ":"); dir && (all || !found); dir = strtok(NULL, ":")) {

                char full[PATH_MAX];
                struct stat st;

                join(dir, argv[i], full, sizeof(full));

                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {

                    printf("%s\n", full);
                    found = 1;
                }
            }
        }

        if (!found)
            failed = 1;
    }

    return failed;
}

COMMAND(clear) {

    (void)argc;
    (void)argv;

    /* move to the top left, clear the screen and the scroll-back */
    printf("\033[H\033[2J\033[3J");

    return 0;
}

COMMAND(sync) {

    (void)argc;
    (void)argv;

    sync();

    return 0;
}

COMMAND(nproc) {

    (void)argc;
    (void)argv;

    printf("%ld\n", sysconf(_SC_NPROCESSORS_ONLN));

    return 0;
}

COMMAND(tty) {

    (void)argc;
    (void)argv;

    const char *name = ttyname(STDIN_FILENO);

    printf("%s\n", name ? name : "not a tty");

    return name ? 0 : 1;
}

/* free: memory use, read from /proc/meminfo */
COMMAND(free) {

    int flags[128] = {0};
    char *values[128] = {0};
    options(argc, argv, "hmgkb", "", flags, values);

    long long total = 0, avail = 0, freem = 0, buffers = 0, cached = 0, shared = 0, sreclaim = 0;
    long long swap_total = 0, swap_free = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    char key[64];
    long long v;

    while (f && fscanf(f, "%63s %lld kB", key, &v) == 2) {

        if (!strcmp(key, "MemTotal:")) total = v;
        else if (!strcmp(key, "MemFree:")) freem = v;
        else if (!strcmp(key, "MemAvailable:")) avail = v;
        else if (!strcmp(key, "Buffers:")) buffers = v;
        else if (!strcmp(key, "Cached:")) cached = v;
        else if (!strcmp(key, "Shmem:")) shared = v;
        else if (!strcmp(key, "SReclaimable:")) sreclaim = v;
        else if (!strcmp(key, "SwapTotal:")) swap_total = v;
        else if (!strcmp(key, "SwapFree:")) swap_free = v;
    }

    if (f)
        fclose(f);

    long long cache = buffers + cached + sreclaim;
    long long used = total - freem - cache;
    long long row1[6] = {total, used, freem, shared, cache, avail};
    long long row2[3] = {swap_total, swap_total - swap_free, swap_free};

    printf("%19s %11s %11s %11s %11s %11s\n", "total", "used", "free", "shared", "buff/cache", "available");

    for (int r = 0; r < 2; r++) {

        printf(r == 0 ? "Mem:   " : "Swap:  ");

        for (int k = 0; k < (r == 0 ? 6 : 3); k++) {

            long long kb = r == 0 ? row1[k] : row2[k];
            char s[32];

            if (flags['h']) human(kb * 1024, s, sizeof(s));
            else if (flags['m']) snprintf(s, sizeof(s), "%lld", kb / 1024);
            else if (flags['g']) snprintf(s, sizeof(s), "%lld", kb / 1024 / 1024);
            else if (flags['b']) snprintf(s, sizeof(s), "%lld", kb * 1024);
            else snprintf(s, sizeof(s), "%lld", kb);

            printf(" %11s", s);
        }

        printf("\n");
    }

    return 0;
}

/* dmesg: the kernel's own messages */
COMMAND(dmesg) {

    (void)argc;
    (void)argv;

    int size = klogctl(10, NULL, 0);   /* SYSLOG_ACTION_SIZE_BUFFER */

    if (size <= 0)
        size = 1 << 17;

    char *buf = malloc((size_t)size + 1);
    int n = buf ? klogctl(3, buf, size) : -1;   /* SYSLOG_ACTION_READ_ALL */

    if (n < 0)
        return fail("cannot read the kernel messages: %s (try: sudo dmesg)", strerror(errno));

    buf[n] = 0;

    /* lines start with "<6>": the importance; leave it out */
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {

        if (line[0] == '<' && strchr(line, '>'))
            line = strchr(line, '>') + 1;

        printf("%s\n", line);
    }

    free(buf);

    return 0;
}

/* mount: list; mount [-t TYPE] [-o OPTIONS] DEVICE FOLDER */
COMMAND(mount) {

    int flags[128] = {0};
    char *values[128] = {0};
    int i = options(argc, argv, "toa", "to", flags, values);

    /* mount -a: everything in /etc/fstab */
    if (flags['a'])
        return mount_fstab(1, 0) ? 1 : 0;

    if (i >= argc) {

        FILE *f = fopen("/proc/mounts", "r");
        char dev[256], dir[256], type[64], opts[512];

        while (f && fscanf(f, "%255s %255s %63s %511s %*d %*d", dev, dir, type, opts) == 4)
            printf("%s on %s type %s (%s)\n", dev, dir, type, opts);

        if (f)
            fclose(f);

        return 0;
    }

    if (argc - i != 2)
        return fail("usage: mount [-t TYPE] [-o ro|rw|remount] DEVICE FOLDER, or mount -a");

    /* UUID=..., LABEL=... work like on other Linux systems */
    char dev[256];

    if (strchr(argv[i], '=') && argv[i][0] != '/') {

        if (resolve_device(argv[i], dev, sizeof(dev)) != 0)
            return fail("%s: no such disk part", argv[i]);

        argv[i] = dev;
    }

    unsigned long mflags = 0;
    char data[256] = "";

    if (values['o']) {

        char copy[256];

        snprintf(copy, sizeof(copy), "%s", values['o']);

        for (char *o = strtok(copy, ","); o; o = strtok(NULL, ",")) {

            if (!strcmp(o, "ro")) mflags |= MS_RDONLY;
            else if (!strcmp(o, "rw")) { /* default */ }
            else if (!strcmp(o, "remount")) mflags |= MS_REMOUNT;
            else if (!strcmp(o, "noexec")) mflags |= MS_NOEXEC;
            else if (!strcmp(o, "nosuid")) mflags |= MS_NOSUID;
            else if (!strcmp(o, "bind")) mflags |= MS_BIND;
            else {

                if (data[0])
                    strncat(data, ",", sizeof(data) - strlen(data) - 1);

                strncat(data, o, sizeof(data) - strlen(data) - 1);
            }
        }
    }

    /* without -t, try the usual types one after the other */
    const char *types[] = {"ext4", "vfat", "iso9660", "ext3", "ext2", "exfat", "ntfs3", "tmpfs", NULL};
    const char *one[] = {values['t'], NULL};
    const char **try = values['t'] ? one : types;

    for (int k = 0; try[k]; k++) {

        if (mount(argv[i], argv[i + 1], try[k], mflags, data[0] ? data : NULL) == 0)
            return 0;
    }

    return fail("cannot mount %s on %s: %s", argv[i], argv[i + 1], strerror(errno));
}

COMMAND(umount) {

    int failed = 0;

    if (argc < 2)
        return fail("usage: umount FOLDER...");

    for (int i = 1; i < argc; i++) {

        if (umount(argv[i]) != 0)
            failed = fail("cannot unmount %s: %s", argv[i], strerror(errno));
    }

    return failed;
}

/* ---------- finding processes by name ---------- */

/* calls fn(pid) for every process named name; returns how many */
static int each_process(const char *name, int full, void (*fn)(pid_t, void *), void *ctx) {

    DIR *d = opendir("/proc");
    struct dirent *e;
    int count = 0;
    pid_t me = getpid();

    while (d && (e = readdir(d)) != NULL) {

        pid_t pid = atoi(e->d_name);

        if (pid <= 0 || pid == me)
            continue;

        char path[64], comm[256] = "";

        snprintf(path, sizeof(path), full ? "/proc/%d/cmdline" : "/proc/%d/comm", pid);

        FILE *f = fopen(path, "r");

        if (!f)
            continue;

        size_t n = fread(comm, 1, sizeof(comm) - 1, f);

        fclose(f);
        comm[n] = 0;

        for (size_t k = 0; k + 1 < n; k++)
            if (comm[k] == 0) comm[k] = ' ';

        comm[strcspn(comm, "\n")] = 0;

        int hit = full ? strstr(comm, name) != NULL : strcmp(comm, name) == 0;

        if (hit) {

            fn(pid, ctx);
            count++;
        }
    }

    if (d)
        closedir(d);

    return count;
}

static void send_signal(pid_t pid, void *ctx) {

    if (kill(pid, *(int *)ctx) != 0)
        fail("(%d): %s", (int)pid, strerror(errno));
}

static int signal_number(const char *s) {

    static const struct { const char *name; int num; } sigs[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"TERM", SIGTERM},
        {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"STOP", SIGSTOP}, {"CONT", SIGCONT},
    };

    if (isdigit((unsigned char)s[0]))
        return atoi(s);

    if (strncmp(s, "SIG", 3) == 0)
        s += 3;

    for (size_t k = 0; k < sizeof(sigs) / sizeof(*sigs); k++)
        if (strcmp(s, sigs[k].name) == 0)
            return sigs[k].num;

    return -1;
}

static int kill_by_name(int argc, char **argv, int full) {

    int sig = SIGTERM, i = 1;

    if (i < argc && argv[i][0] == '-' && argv[i][1]) {

        sig = signal_number(argv[i] + 1);

        if (sig < 0)
            return fail("unknown signal %s", argv[i]);

        i++;
    }

    if (i >= argc)
        return fail("usage: %s [-SIGNAL] NAME...", prog);

    int failed = 0;

    for (; i < argc; i++) {

        if (each_process(argv[i], full, send_signal, &sig) == 0)
            failed = full ? 1 : fail("%s: no process found", argv[i]);
    }

    return failed;
}

COMMAND(killall) { return kill_by_name(argc, argv, 0); }
COMMAND(pkill) { return kill_by_name(argc, argv, 1); }

static void print_pid(pid_t pid, void *ctx) {

    int *first = ctx;

    printf(*first ? "%d" : " %d", (int)pid);
    *first = 0;
}

COMMAND(pidof) {

    int first = 1, found = 0;

    for (int i = 1; i < argc; i++)
        found += each_process(argv[i], 0, print_pid, &first);

    if (found)
        printf("\n");

    return found ? 0 : 1;
}

COMMAND(pgrep) {

    int found = 0, first = 1;

    for (int i = 1; i < argc; i++)
        found += each_process(argv[i], 1, print_pid, &first);

    if (found)
        printf("\n");

    return found ? 0 : 1;
}

/* ---------- hardware ---------- */

COMMAND(lscpu) {

    (void)argc;
    (void)argv;

    struct utsname u;
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512], model[256] = "unknown", vendor[128] = "unknown", mhz[64] = "";

    uname(&u);

    while (f && fgets(line, sizeof(line), f)) {

        char *colon = strchr(line, ':');

        if (!colon)
            continue;

        char *v = colon + 2;

        v[strcspn(v, "\n")] = 0;

        if (!strncmp(line, "model name", 10) && !strcmp(model, "unknown")) snprintf(model, sizeof(model), "%s", v);
        if (!strncmp(line, "vendor_id", 9)) snprintf(vendor, sizeof(vendor), "%s", v);
        if (!strncmp(line, "cpu MHz", 7) && !mhz[0]) snprintf(mhz, sizeof(mhz), "%s", v);
    }

    if (f)
        fclose(f);

    printf("Architecture:    %s\n", u.machine);
    printf("CPU(s):          %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    printf("Vendor ID:       %s\n", vendor);
    printf("Model name:      %s\n", model);

    if (mhz[0])
        printf("CPU MHz:         %s\n", mhz);

    return 0;
}

COMMAND(lsmod) {

    (void)argc;
    (void)argv;

    FILE *f = fopen("/proc/modules", "r");
    char name[128], used_by[512];
    long size;
    int count;

    if (!f)
        return fail("cannot read /proc/modules");

    printf("%-24s %8s  %s\n", "Module", "Size", "Used by");

    while (fscanf(f, "%127s %ld %d %511s %*s %*s", name, &size, &count, used_by) == 4) {

        if (strcmp(used_by, "-") == 0)
            used_by[0] = 0;

        size_t n = strlen(used_by);

        if (n && used_by[n - 1] == ',')
            used_by[n - 1] = 0;

        printf("%-24s %8ld  %d %s\n", name, size, count, used_by);
    }

    fclose(f);

    return 0;
}
