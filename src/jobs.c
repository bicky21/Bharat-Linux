#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int process_alive(int pid) {

    char path[256];

    snprintf(path,
             sizeof(path),
             "/proc/%d/stat",
             pid);

    FILE *f = fopen(path, "r");

    if (!f)
        return 0;

    int proc_pid;

    char comm[128];

    char state;

    fscanf(f,
           "%d %127s %c",
           &proc_pid,
           comm,
           &state);

    fclose(f);

    if (state == 'Z')
        return 0;

    return 1;
}

int main() {

    FILE *f = fopen("/tmp/jobs", "r");

    if (!f) {

        printf("No jobs\n");

        return 0;
    }

    FILE *tmp = fopen("/tmp/jobs.tmp", "w");

    if (!tmp) {

        fclose(f);

        return 1;
    }

    char line[256];

    int found = 0;

    while (fgets(line, sizeof(line), f)) {

        int pid;

        if (sscanf(line,
                   "[%d]",
                   &pid) != 1)
            continue;

        if (process_alive(pid)) {

            printf("%s", line);

            fprintf(tmp, "%s", line);

            found = 1;
        }
    }

    fclose(f);
    fclose(tmp);

    remove("/tmp/jobs");

    rename("/tmp/jobs.tmp",
           "/tmp/jobs");

    if (!found)
        printf("No active jobs\n");

    return 0;
}
