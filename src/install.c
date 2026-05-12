#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: install <package>\n");

        return 1;
    }

    FILE *pkg = fopen(argv[1], "r");

    if (!pkg) {

        printf("Cannot open package\n");

        return 1;
    }

    char line[256];

    while (fgets(line, sizeof(line), pkg)) {

        line[strcspn(line, "\n")] = 0;

        char *target = strtok(line, "|");

        char *source = strtok(NULL, "|");

        if (!target || !source)
            continue;

        FILE *src = fopen(source, "rb");

        if (!src) {

            printf("Missing source: %s\n", source);

            continue;
        }

        FILE *dst = fopen(target, "wb");

        if (!dst) {

            printf("Cannot write: %s\n", target);

            fclose(src);

            continue;
        }

        char buffer[1024];

        int bytes;

        while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {

            fwrite(buffer, 1, bytes, dst);
        }

        fclose(src);
        fclose(dst);

        chmod(target, 0755);

        printf("Installed %s\n", target);
    }

    fclose(pkg);

    return 0;
}
