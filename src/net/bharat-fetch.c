/*
 * bharat-fetch - the browser's helper: it does the dangerous work in its
 * own process, so a bad web page or picture cannot crash the desktop.
 *
 *   - fetches pages (http and https, cookies kept for the session)
 *   - reads pictures (PNG, JPEG, GIF, BMP) and hands over plain pixels
 *   - saves downloads
 *
 * The browser talks to it over standard input and output, one request
 * at a time:
 *
 *   in:   ID PAGE URL\n                     a page
 *         ID POST URL LENGTH\n BODY          a form
 *         ID IMAGE URL MAXW MAXH\n          a picture, made smaller to fit
 *         ID SAVE URL PATH\n                a download
 *   out:  ID PAGE STATUS LEN FINAL-URL\tCONTENT-TYPE\n BODY
 *         ID IMAGE W H LEN\n RGBA-PIXELS
 *         ID SAVED BYTES PATH\n
 *         ID ERROR message\n
 */

#include "../libhttp.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static void put_all(const void *data, size_t len) {

    const char *p = data;

    while (len) {

        ssize_t n = write(STDOUT_FILENO, p, len);

        if (n <= 0)
            exit(0);         /* the browser went away */

        p += n;
        len -= (size_t)n;
    }
}

static void reply_error(long id, const char *msg) {

    char line[400];
    int n = snprintf(line, sizeof(line), "%ld ERROR %.300s\n", id, msg);

    put_all(line, (size_t)n);
}

/* reads one line from the browser */
static int get_line(char *out, size_t size) {

    size_t k = 0;
    char c;

    while (read(STDIN_FILENO, &c, 1) == 1) {

        if (c == '\n') {

            out[k] = 0;
            return 0;
        }

        if (k + 1 < size)
            out[k++] = c;
    }

    return -1;
}

static void page(long id, const char *url, const char *body) {

    struct http_result r;

    if (http_fetch(url, body, NULL, NULL, &r) != 0) {

        reply_error(id, r.error);
        return;
    }

    char head[4600];
    int n = snprintf(head, sizeof(head), "%ld PAGE %d %zu %s\t%s\n", id, r.status, r.len,
                     r.final_url, r.content_type[0] ? r.content_type : "text/html");

    put_all(head, (size_t)n);
    put_all(r.body ? r.body : "", r.len);
    free(r.body);
}

/* a picture, made smaller (never bigger) to fit max_w x max_h */
static void image(long id, const char *url, int max_w, int max_h) {

    struct http_result r;

    if (http_fetch(url, NULL, NULL, NULL, &r) != 0 || r.status != 200 || !r.body) {

        reply_error(id, r.error[0] ? r.error : "the picture is not there");
        free(r.body);
        return;
    }

    int w, h, ch;
    unsigned char *px = stbi_load_from_memory((unsigned char *)r.body, (int)r.len, &w, &h, &ch, 4);

    free(r.body);

    if (!px) {

        reply_error(id, "this picture format is not supported");
        return;
    }

    int nw = w, nh = h;

    if (max_w > 0 && nw > max_w) { nh = (int)((long long)nh * max_w / nw); nw = max_w; }
    if (max_h > 0 && nh > max_h) { nw = (int)((long long)nw * max_h / nh); nh = max_h; }
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    unsigned char *out = px;

    /* box filter: each new pixel is the average of the old ones it covers */
    if (nw != w || nh != h) {

        out = malloc((size_t)nw * (size_t)nh * 4);

        if (!out) {

            stbi_image_free(px);
            reply_error(id, "out of memory");
            return;
        }

        for (int y = 0; y < nh; y++) {

            int y0 = (int)((long long)y * h / nh), y1 = (int)((long long)(y + 1) * h / nh);

            if (y1 <= y0) y1 = y0 + 1;

            for (int x = 0; x < nw; x++) {

                int x0 = (int)((long long)x * w / nw), x1 = (int)((long long)(x + 1) * w / nw);
                unsigned sum[4] = {0, 0, 0, 0}, count = 0;

                if (x1 <= x0) x1 = x0 + 1;

                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {

                        unsigned char *p = px + ((size_t)yy * (size_t)w + (size_t)xx) * 4;

                        for (int k = 0; k < 4; k++)
                            sum[k] += p[k];
                        count++;
                    }

                for (int k = 0; k < 4; k++)
                    out[((size_t)y * (size_t)nw + (size_t)x) * 4 + (size_t)k] = (unsigned char)(sum[k] / count);
            }
        }

        stbi_image_free(px);
    }

    char head[100];
    size_t len = (size_t)nw * (size_t)nh * 4;
    int n = snprintf(head, sizeof(head), "%ld IMAGE %d %d %zu\n", id, nw, nh, len);

    put_all(head, (size_t)n);
    put_all(out, len);

    if (out != px)
        free(out);
    else
        stbi_image_free(px);
}

static void save(long id, const char *url, const char *path) {

    char part[4200];

    snprintf(part, sizeof(part), "%.4000s.part", path);

    FILE *f = fopen(part, "w");
    struct http_result r;

    if (!f) {

        reply_error(id, strerror(errno));
        return;
    }

    int failed = http_fetch(url, NULL, NULL, f, &r) != 0;

    fclose(f);

    if (failed || r.status != 200) {

        unlink(part);
        reply_error(id, failed ? r.error : "the server did not send the file");
        return;
    }

    rename(part, path);

    char line[4300];
    int n = snprintf(line, sizeof(line), "%ld SAVED %zu %.4000s\n", id, r.len, path);

    put_all(line, (size_t)n);
}

int main(void) {

    signal(SIGPIPE, SIG_IGN);

    char line[8400];

    while (get_line(line, sizeof(line)) == 0) {

        long id;
        char kind[16], url[4200], extra[4200] = "";
        int a = 0, b = 0;

        if (sscanf(line, "%ld %15s %4199s", &id, kind, url) != 3)
            continue;

        if (!strcmp(kind, "PAGE")) {

            page(id, url, NULL);
        }
        else if (!strcmp(kind, "POST")) {

            size_t len = 0;

            sscanf(line, "%*d %*s %*s %zu", &len);

            char *body = len < 1024 * 1024 ? malloc(len + 1) : NULL;
            size_t got = 0;

            while (body && got < len) {

                ssize_t n = read(STDIN_FILENO, body + got, len - got);

                if (n <= 0)
                    return 0;

                got += (size_t)n;
            }

            if (!body) {

                reply_error(id, "the form is too big");
                continue;
            }

            body[len] = 0;
            page(id, url, body);
            free(body);
        }
        else if (!strcmp(kind, "IMAGE")) {

            sscanf(line, "%*d %*s %*s %d %d", &a, &b);
            image(id, url, a, b);
        }
        else if (!strcmp(kind, "SAVE")) {

            const char *p = line;

            /* the path is the rest of the line (it may contain spaces) */
            for (int field = 0; field < 3 && p; field++) {

                p = strchr(p, ' ');

                if (p)
                    p++;
            }

            snprintf(extra, sizeof(extra), "%s", p ? p : "");
            save(id, url, extra);
        }
        else
            reply_error(id, "unknown request");
    }

    return 0;
}
