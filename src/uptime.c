#include <stdio.h>
#include <time.h>

int main() {

    FILE *f = fopen("/proc/uptime", "r");

    if (!f) {

        printf("Cannot read uptime\n");

        return 1;
    }

    double up;

    fscanf(f, "%lf", &up);

    fclose(f);

    int hours = up / 3600;
    int mins = ((int)up % 3600) / 60;
    int secs = (int)up % 60;

    printf("Uptime: %02d:%02d:%02d\n",
           hours,
           mins,
           secs);

    return 0;
}
