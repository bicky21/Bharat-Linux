#ifndef LIBSHUTDOWN_H
#define LIBSHUTDOWN_H

/*
 * A clean shutdown, used by poweroff and reboot (as root):
 *   1. ask every program to stop (SIGTERM), so they can save their work
 *   2. wait up to 5 seconds for them
 *   3. stop the rest by force (SIGKILL)
 *   4. write everything to disk and make the disk read-only
 * Then the caller switches the computer off or restarts it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mount.h>

/* Normal programs still running, apart from init and ourselves. */
static inline int programs_left(void) {

    DIR *d = opendir("/proc");
    struct dirent *e;
    int count = 0;
    pid_t me = getpid();

    while (d && (e = readdir(d)) != NULL) {

        pid_t pid = atoi(e->d_name);

        if (pid <= 1 || pid == me)
            continue;

        /* kernel threads have an empty command line: skip them */
        char path[64], c;

        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

        FILE *f = fopen(path, "r");

        if (f) {

            if (fread(&c, 1, 1, f) == 1)
                count++;

            fclose(f);
        }
    }

    if (d)
        closedir(d);

    return count;
}

static inline void shut_down_cleanly(void) {

    /* our own terminal may go away: do not let that stop us */
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    printf("Stopping all programs...\n");
    fflush(stdout);

    kill(-1, SIGTERM);

    for (int i = 0; i < 50 && programs_left() > 0; i++)
        usleep(100 * 1000);

    kill(-1, SIGKILL);
    usleep(200 * 1000);

    printf("Saving data to disk...\n");
    fflush(stdout);

    sync();

    /* make the disk read-only so nothing is half-written */
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    sync();
}

#endif
