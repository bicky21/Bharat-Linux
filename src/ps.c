#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

int main() {

    DIR *d = opendir("/proc");

    if (!d) {

        printf("Cannot open /proc\n");

        return 1;
    }

    struct dirent *dir;

    printf("PID STATE COMMAND\n");
    printf("-----------------\n");

    while ((dir = readdir(d))) {

        if (!isdigit((unsigned char)dir->d_name[0]))
            continue;

        char path[512];

        snprintf(path,
                 sizeof(path),
                 "/proc/%.200s/stat",
                 dir->d_name);

        FILE *f = fopen(path, "r");

        if (!f)
            continue;

        int pid;

        char comm[128];

        char state;

        fscanf(f,
               "%d %127s %c",
               &pid,
               comm,
               &state);

        fclose(f);

        printf("%d %c %s\n",
               pid,
               state,
               comm);
    }

    closedir(d);

    return 0;
}
