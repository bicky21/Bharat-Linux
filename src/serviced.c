#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int main() {

    printf("Bharat Service Manager Started\n");

    while (1) {

        FILE *f = fopen("/etc/services/enabled", "r");

        if (f) {

            char line[256];

            while (fgets(line, sizeof(line), f)) {

                line[strcspn(line, "\n")] = 0;

                if (strlen(line) > 0)
                    system(line);
            }

            fclose(f);
        }

        sleep(60);
    }

    return 0;
}
