/*
 * wget - download a file (http:// and https://).
 *
 *   wget URL               saves it under its own name
 *   wget -O FILE URL       saves it as FILE (-O - prints it)
 *   wget -q ...            quietly
 */

#include "../libhttp.h"

int main(int argc, char **argv) {

    const char *out_name = NULL, *address = NULL;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {

        if (!strcmp(argv[i], "-O") && i + 1 < argc) out_name = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "wget: unknown option %s\n", argv[i]); return 2; }
        else address = argv[i];
    }

    if (!address) {

        fprintf(stderr, "usage: wget [-O FILE] [-q] URL\n");
        return 2;
    }

    /* the file name: the last part of the address */
    struct url u;
    char name[256];

    if (url_parse(address, &u) != 0) {

        fprintf(stderr, "wget: %s: not an http:// or https:// address\n", address);
        return 2;
    }

    const char *last = strrchr(u.path, '/');

    snprintf(name, sizeof(name), "%s", last && last[1] ? last + 1 : "index.html");
    name[strcspn(name, "?#")] = 0;

    if (!name[0])
        snprintf(name, sizeof(name), "index.html");

    if (!out_name)
        out_name = name;

    int to_stdout = !strcmp(out_name, "-");
    char part[300];

    snprintf(part, sizeof(part), "%s.part", out_name);

    FILE *f = to_stdout ? stdout : fopen(part, "w");

    if (!f) {

        fprintf(stderr, "wget: %s: %s\n", part, strerror(errno));
        return 1;
    }

    if (!quiet && !to_stdout)
        fprintf(stderr, "Downloading %s ...\n", address);

    struct http_result r;
    int failed = http_fetch(address, NULL, NULL, f, &r) != 0;

    if (!to_stdout)
        fclose(f);

    if (failed || r.status != 200) {

        if (failed)
            fprintf(stderr, "wget: %s\n", r.error);
        else
            fprintf(stderr, "wget: the server answered %d\n", r.status);

        if (!to_stdout)
            unlink(part);

        return 1;
    }

    if (!to_stdout && rename(part, out_name) != 0) {

        fprintf(stderr, "wget: %s: %s\n", out_name, strerror(errno));
        return 1;
    }

    if (!quiet && !to_stdout) {

        double kb = (double)r.len / 1024;

        fprintf(stderr, "Saved %s (%.1f %s)\n", out_name, kb > 1024 ? kb / 1024 : kb, kb > 1024 ? "MB" : "KB");
    }

    return 0;
}
