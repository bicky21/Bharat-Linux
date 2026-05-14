#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void install_pkg(char *pkg, char *version) {

    FILE *db = fopen("/var/lib/bpm/packages.db", "a");

    FILE *ver = fopen("/var/lib/bpm/versions.db", "a");

    if (!db || !ver) {

        printf("Cannot open package database\n");

        return;
    }

    fprintf(db, "%s\n", pkg);

    fprintf(ver, "%s %s\n", pkg, version);

    fclose(db);
    fclose(ver);

    printf("Installed %s version %s\n",
           pkg,
           version);
}

void list_pkgs() {

    FILE *ver = fopen("/var/lib/bpm/versions.db", "r");

    if (!ver) {

        printf("No packages installed\n");

        return;
    }

    char line[256];

    while (fgets(line, sizeof(line), ver)) {

        printf("%s", line);
    }

    fclose(ver);
}

int main(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage:\n");
        printf("  bpm install <pkg> <version>\n");
        printf("  bpm list\n");

        return 1;
    }

    if (strcmp(argv[1], "install") == 0) {

        if (argc < 4) {

            printf("Missing package/version\n");

            return 1;
        }

        install_pkg(argv[2], argv[3]);
    }

    else if (strcmp(argv[1], "list") == 0) {

        list_pkgs();
    }

    else {

        printf("Unknown command\n");
    }

    return 0;
}
