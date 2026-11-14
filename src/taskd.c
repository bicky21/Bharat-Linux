#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main() {

    while (1) {

        FILE *f = fopen("/etc/tasks/tasks", "r");

        int tasks = 0;

        if (f) {

            char line[256];

            while (fgets(line, sizeof(line), f)) {

                line[strcspn(line, "\n")] = 0;

                if (strlen(line) == 0 || line[0] == '#')
                    continue;

                tasks++;

                if (system(line) != 0)
                    fprintf(stderr, "taskd: task failed: %s\n", line);
            }

            fclose(f);
        }

        /* nothing to do: look at the list again only every 5 minutes */
        sleep(tasks ? 30 : 300);
    }

    return 0;
}
