#ifndef LIBURL_H
#define LIBURL_H

/*
 * liburl.h - web addresses: reading them, and turning a link on a page
 * ("../a.html") into a full address. Used by libhttp.h and the browser.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ====================================================================
 * addresses (URLs)
 * ==================================================================== */

struct url {
    int https;
    char host[256];
    char port[8];
    char path[4096];       /* with ?query, without #fragment */
};

static inline int url_parse(const char *text, struct url *u) {

    memset(u, 0, sizeof(*u));

    while (isspace((unsigned char)*text))
        text++;

    if (!strncasecmp(text, "https://", 8)) { u->https = 1; text += 8; }
    else if (!strncasecmp(text, "http://", 7)) text += 7;
    else if (strstr(text, "://")) return -1;          /* ftp:, file: ... */

    size_t hl = strcspn(text, "/:?#");

    if (hl == 0 || hl >= sizeof(u->host))
        return -1;

    memcpy(u->host, text, hl);

    for (char *c = u->host; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    text += hl;
    snprintf(u->port, sizeof(u->port), "%s", u->https ? "443" : "80");

    if (*text == ':') {

        size_t pl = strcspn(text + 1, "/?#");

        if (pl == 0 || pl > 5)
            return -1;

        snprintf(u->port, sizeof(u->port), "%.*s", (int)pl, text + 1);
        text += 1 + pl;
    }

    size_t rest = strcspn(text, "#");

    if (rest == 0)
        snprintf(u->path, sizeof(u->path), "/");
    else if (*text == '?')
        snprintf(u->path, sizeof(u->path), "/%.*s", (int)(rest < 4000 ? rest : 4000), text);
    else
        snprintf(u->path, sizeof(u->path), "%.*s", (int)(rest < 4090 ? rest : 4090), text);

    /* spaces are not allowed in addresses */
    for (char *c = u->path; *c; c++)
        if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
            *c = '+';

    return 0;
}

static inline void url_text(const struct url *u, char *out, size_t size) {

    int normal = !strcmp(u->port, u->https ? "443" : "80");

    snprintf(out, size, "%s://%s%s%s%.3900s", u->https ? "https" : "http", u->host,
             normal ? "" : ":", normal ? "" : u->port, u->path);
}

/* "../img/a.png" on https://x.org/docs/page.html -> https://x.org/img/a.png */
static inline int url_resolve(const char *base, const char *ref, char *out, size_t size) {

    struct url b, r;

    while (isspace((unsigned char)*ref))
        ref++;

    if (!strncasecmp(ref, "http://", 7) || !strncasecmp(ref, "https://", 8)) {

        if (url_parse(ref, &r) != 0)
            return -1;

        url_text(&r, out, size);
        return 0;
    }

    if (strchr(ref, ':') && strcspn(ref, ":") < strcspn(ref, "/?#"))
        return -1;             /* mailto:, javascript:, data: ... */

    if (url_parse(base, &b) != 0)
        return -1;

    char path[4096];

    if (!strncmp(ref, "//", 2)) {

        char full[4200];

        snprintf(full, sizeof(full), "%s:%.4000s", b.https ? "https" : "http", ref);
        return url_parse(full, &r) == 0 ? (url_text(&r, out, size), 0) : -1;
    }

    if (ref[0] == '/') {

        snprintf(path, sizeof(path), "%.4000s", ref);
    }
    else if (ref[0] == '?') {

        snprintf(path, sizeof(path), "%.*s%.2000s", (int)strcspn(b.path, "?"), b.path, ref);
    }
    else if (ref[0] == '#' || ref[0] == 0) {

        snprintf(path, sizeof(path), "%s", b.path);
    }
    else {

        /* the folder of the base page */
        char dir[4096];
        size_t q = strcspn(b.path, "?");

        snprintf(dir, sizeof(dir), "%.*s", (int)q, b.path);

        char *slash = strrchr(dir, '/');

        if (slash)
            slash[1] = 0;

        snprintf(path, sizeof(path), "%.2000s%.2000s", dir, ref);
    }

    path[strcspn(path, "#")] = 0;

    /* remove "./" and "x/../" */
    char clean[4096] = "";
    char *q = strchr(path, '?');
    char query[4096] = "";

    if (q) {

        snprintf(query, sizeof(query), "%s", q);
        *q = 0;
    }

    char *parts[512];
    int n = 0;
    int trailing = path[0] && path[strlen(path) - 1] == '/';

    for (char *p = strtok(path, "/"); p && n < 512; p = strtok(NULL, "/")) {

        if (!strcmp(p, "."))
            continue;

        if (!strcmp(p, "..")) {

            if (n > 0)
                n--;
            continue;
        }

        parts[n++] = p;
    }

    for (int i = 0; i < n; i++) {

        strncat(clean, "/", sizeof(clean) - strlen(clean) - 1);
        strncat(clean, parts[i], sizeof(clean) - strlen(clean) - 1);
    }

    if (!clean[0] || trailing)
        strncat(clean, "/", sizeof(clean) - strlen(clean) - 1);

    strncat(clean, query, sizeof(clean) - strlen(clean) - 1);
    snprintf(b.path, sizeof(b.path), "%s", clean);
    url_text(&b, out, size);

    return 0;
}

#endif
