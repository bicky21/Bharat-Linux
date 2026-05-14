#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>

int is_numeric(char *s) {

    for (int i = 0; s[i]; i++) {

        if (!isdigit(s[i]))
            return 0;
    }

    return 1;
}

int main() {

    DIR *d = opendir("/proc");

    if (!d) {

        printf("Cannot open /proc\n");

        return 1;
    }

    struct dirent *dir;

    printf("PID CMD\n");
    printf("-------\n");

    while ((dir = readdir(d)) != NULL) {

        if (!is_numeric(dir->d_name))
            continue;

        char path[512];

        snprintf(path,
                 sizeof(path),
                 "/proc/%s/comm",
                 dir->d_name);

        FILE *f = fopen(path, "r");

        if (!f)
            continue;

        char cmd[256];

        if (!fgets(cmd, sizeof(cmd), f)) {

            fclose(f);

            continue;
        }

        cmd[strcspn(cmd, "\n")] = 0;

        fclose(f);

        printf("%s %s\n",
               dir->d_name,
               cmd);
    }

    closedir(d);

    return 0;
}
