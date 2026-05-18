#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {

    FILE *f = fopen("/run/processes.db", "r");

    if (!f) {

        printf("Cannot open process database\n");

        return 1;
    }

    printf("PID USER COMMAND\n");
    printf("------------------------------\n");

    char line[512];

    while (fgets(line,
                 sizeof(line),
                 f)) {

        int pid;

        char user[64];
        char cmd[256];

        if (sscanf(line,
                   "%d:%63[^:]:%255[^\n]",
                   &pid,
                   user,
                   cmd) != 3)
            continue;

        printf("%d %s %s\n",
               pid,
               user,
               cmd);
    }

    fclose(f);

    return 0;
}
