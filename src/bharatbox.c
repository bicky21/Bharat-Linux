#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>

void cmd_pwd() {

    char cwd[256];

    getcwd(cwd, sizeof(cwd));

    printf("%s\n", cwd);
}

void cmd_echo(int argc, char *argv[]) {

    for (int i = 1; i < argc; i++) {

        printf("%s ", argv[i]);
    }

    printf("\n");
}

void cmd_whoami() {

    char *u = getenv("USER");

    if (!u)
        u = "unknown";

    printf("%s\n", u);
}

void cmd_ls(int argc, char *argv[]) {

    char *path = ".";

    if (argc > 1)
        path = argv[1];

    DIR *d = opendir(path);

    if (!d) {

        printf("Cannot open directory\n");

        return;
    }

    struct dirent *dir;

    while ((dir = readdir(d)) != NULL) {

        printf("%s\n", dir->d_name);
    }

    closedir(d);
}

void cmd_cat(int argc, char *argv[]) {

    if (argc < 2) {

        printf("Usage: cat <file>\n");

        return;
    }

    FILE *f = fopen(argv[1], "r");

    if (!f) {

        printf("Cannot open file\n");

        return;
    }

    char c;

    while ((c = fgetc(f)) != EOF)
        putchar(c);

    fclose(f);
}

int main(int argc, char *argv[]) {

    char *name = strrchr(argv[0], '/');

    if (name)
        name++;
    else
        name = argv[0];

    if (strcmp(name, "pwd") == 0) {

        cmd_pwd();
    }

    else if (strcmp(name, "echo") == 0) {

        cmd_echo(argc, argv);
    }

    else if (strcmp(name, "whoami") == 0) {

        cmd_whoami();
    }

    else if (strcmp(name, "ls") == 0) {

        cmd_ls(argc, argv);
    }

    else if (strcmp(name, "cat") == 0) {

        cmd_cat(argc, argv);
    }

    else {

        printf("Unknown applet: %s\n", name);
    }

    return 0;
}
