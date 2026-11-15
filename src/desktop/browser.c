/*
 * browser.c - Bharat Browser: a web browser written from scratch.
 *
 * Tabs, back/forward, an address bar that also searches, bookmarks,
 * history, forms, pictures, downloads, find in page, page source.
 *
 * The dangerous work (network, encryption, decoding pictures) is done by
 * a helper program, bharat-fetch, in its own process: a bad page cannot
 * crash the desktop. This window only reads its answers, so the desktop
 * never waits for the network.
 *
 * No JavaScript (a reading browser): pages that need it show what they
 * can. Searches go to DuckDuckGo's HTML version, which needs none.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "desktop.h"
#include "layout.h"
#include "../liburl.h"

#define MAX_TABS   8
#define MAX_HIST   64
#define TABS_H     30
#define TOOL_H     40
#define STATUS_H   22
#define BTN        32
#define HOME_URL   "about:home"
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="

struct tab {
    struct page page;
    char url[4200];
    char *hist[MAX_HIST];
    int nhist, at;            /* back/forward list, current position */
    int scroll;
    int loading;
    char *source;             /* the page's HTML, for "View page source" */
    size_t source_len;
};

enum { R_PAGE, R_IMAGE, R_SAVE };

struct request {
    long id;
    struct tab *tab;
    int kind;
    int image;
    int used;
};

struct browser {
    struct tab *tabs[MAX_TABS];
    int ntabs, cur;
    char address[4200];
    int editing;              /* typing in the address bar */
    int replace_all;          /* the next key replaces the whole address */
    pid_t pid;
    int to_fetcher, from_fetcher;
    char *in;
    size_t in_len, in_cap;
    struct request req[256];
    long next_id;
    char status[300];
    char hover[300];
    char find[128];
    int find_y;
};

static struct window *the_window;

/* ====================================================================
 * small helpers
 * ==================================================================== */

static struct tab *cur_tab(struct browser *b) { return b->tabs[b->cur]; }

static void config_path(const char *name, char *out, size_t size) {

    const char *home = getenv("HOME");
    char dir[512];

    snprintf(dir, sizeof(dir), "%s/.config", home ? home : "/tmp");
    mkdir(dir, 0700);
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "/bharat");
    mkdir(dir, 0700);
    snprintf(out, size, "%s/%s", dir, name);
}

static void url_encode(const char *in, char *out, size_t size) {

    size_t o = 0;

    for (; *in && o + 4 < size; in++) {

        unsigned char c = (unsigned char)*in;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else if (c == ' ')
            out[o++] = '+';
        else
            o += (size_t)snprintf(out + o, size - o, "%%%02X", c);
    }

    out[o] = 0;
}

/* "wikipedia.org" -> https://wikipedia.org ; "how to cook" -> a search */
static void address_to_url(const char *typed, char *out, size_t size) {

    while (isspace((unsigned char)*typed))
        typed++;

    if (!strncmp(typed, "about:", 6) || !strncasecmp(typed, "http://", 7) || !strncasecmp(typed, "https://", 8)) {

        snprintf(out, size, "%s", typed);
        return;
    }

    if (strchr(typed, '.') && !strchr(typed, ' ') && typed[0] != '.') {

        snprintf(out, size, "https://%s", typed);
        return;
    }

    char q[1200];

    url_encode(typed, q, sizeof(q));
    snprintf(out, size, SEARCH_URL "%s", q);
}

/* ====================================================================
 * the helper process
 * ==================================================================== */

static void stop_fetcher(struct browser *b) {

    if (b->pid > 0) {

        kill(b->pid, SIGKILL);
        waitpid(b->pid, NULL, 0);
    }

    if (b->to_fetcher >= 0) close(b->to_fetcher);
    if (b->from_fetcher >= 0) close(b->from_fetcher);

    b->pid = -1;
    b->to_fetcher = b->from_fetcher = -1;
    b->in_len = 0;
}

static int start_fetcher(struct browser *b) {

    int up[2], down[2];

    if (pipe2(up, O_CLOEXEC) != 0)
        return -1;

    if (pipe2(down, O_CLOEXEC) != 0) {

        close(up[0]);
        close(up[1]);
        return -1;
    }

    pid_t pid = fork();

    if (pid == 0) {

        dup2(down[0], STDIN_FILENO);
        dup2(up[1], STDOUT_FILENO);
        execl("/bin/bharat-fetch", "bharat-fetch", NULL);
        _exit(127);
    }

    close(down[0]);
    close(up[1]);

    if (pid < 0) {

        close(down[1]);
        close(up[0]);
        return -1;
    }

    fcntl(up[0], F_SETFL, O_NONBLOCK);

    b->pid = pid;
    b->to_fetcher = down[1];
    b->from_fetcher = up[0];

    return 0;
}

static long ask(struct browser *b, struct tab *t, int kind, int image, const char *line_fmt, const char *url,
                const char *extra, const char *body) {

    if (b->pid <= 0 && start_fetcher(b) != 0)
        return -1;

    struct request *r = NULL;

    for (int i = 0; i < 256 && !r; i++)
        if (!b->req[i].used)
            r = &b->req[i];

    if (!r)
        return -1;

    r->used = 1;
    r->id = ++b->next_id;
    r->tab = t;
    r->kind = kind;
    r->image = image;

    char line[8600];
    int n = snprintf(line, sizeof(line), line_fmt, r->id, url, extra ? extra : "");

    if (n >= (int)sizeof(line) || write(b->to_fetcher, line, (size_t)n) != n ||
        (body && write(b->to_fetcher, body, strlen(body)) != (ssize_t)strlen(body))) {

        r->used = 0;
        stop_fetcher(b);
        return -1;
    }

    return r->id;
}

/* asks for the pictures of a page that are not here yet (at most 80) */
static void ask_images(struct browser *b, struct tab *t) {

    int width = t->page.width - 32;

    for (int i = 0; i < t->page.nimages && i < 80; i++) {

        struct page_image *im = &t->page.images[i];

        if (im->state != 0)
            continue;

        char size[40];

        snprintf(size, sizeof(size), "%d %d", im->want_w ? im->want_w : (width > 980 ? 980 : width),
                 im->want_h ? im->want_h : 1600);

        if (ask(b, t, R_IMAGE, i, "%ld IMAGE %s %s\n", im->url, size, NULL) > 0)
            im->state = 1;
    }
}

/* forgets requests for a tab (it navigated away, or closed) */
static void drop_requests(struct browser *b, struct tab *t) {

    for (int i = 0; i < 256; i++)
        if (b->req[i].used && b->req[i].tab == t)
            b->req[i].used = 0;
}

/* ====================================================================
 * pages
 * ==================================================================== */

static int content_width(struct window *win) {

    int x, y, w, h;

    content_box(win, &x, &y, &w, &h);

    return w - 10;      /* the scroll bar */
}

static void show_html(struct browser *b, struct tab *t, const char *url, const char *html, size_t len, int is_html) {

    page_free(&t->page);
    page_load(&t->page, url, html, len, is_html);
    page_layout(&t->page, content_width(the_window));
    t->scroll = 0;
    t->loading = 0;

    free(t->source);
    t->source = malloc(len + 1);

    if (t->source) {

        memcpy(t->source, html, len);
        t->source[len] = 0;
        t->source_len = len;
    }

    snprintf(t->url, sizeof(t->url), "%s", url);

    if (t == cur_tab(b) && !b->editing)
        snprintf(b->address, sizeof(b->address), "%s", url);

    ask_images(b, t);

    if (t == cur_tab(b)) {

        char title[64];

        snprintf(title, sizeof(title), "%.50s", t->page.title[0] ? t->page.title : "Bharat Browser");

        /* the taskbar shows the title too */
        if (strcmp(the_window->title, title))
            wm_redraw();

        snprintf(the_window->title, sizeof(the_window->title), "%s", title);
    }
}

static void html_escape(const char *in, char *out, size_t size) {

    size_t o = 0;

    for (; *in && o + 7 < size; in++) {

        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;" : NULL;

        if (rep) {

            memcpy(out + o, rep, strlen(rep));
            o += strlen(rep);
        }
        else
            out[o++] = *in;
    }

    out[o] = 0;
}

static void show_error(struct browser *b, struct tab *t, const char *url, const char *why) {

    char html[3000], u[1200], w[600];

    html_escape(url, u, sizeof(u));
    html_escape(why, w, sizeof(w));
    snprintf(html, sizeof(html),
             "<title>Cannot open the page</title><h1>This page cannot be opened</h1>"
             "<p><b>%s</b></p><p>%s</p><ul><li>Check the address.</li>"
             "<li>Check the network: the icon in the taskbar, or the command <code>network</code>.</li>"
             "<li>Some sites only work with JavaScript, which Bharat Browser does not run.</li></ul>"
             "<p><a href=\"about:home\">Go to the start page</a></p>", u, w);
    show_html(b, t, url, html, strlen(html), 1);
}

/* the start page, bookmarks and history are made here */
static void about_page(struct browser *b, struct tab *t, const char *url) {

    size_t cap = 64 * 1024, len = 0;
    char *h = malloc(cap);

    if (!h)
        return;

#define ADD(...) do { int n_ = snprintf(h + len, cap - len, __VA_ARGS__); if (n_ > 0 && len + (size_t)n_ < cap) len += (size_t)n_; } while (0)

    if (!strcmp(url, "about:bookmarks") || !strcmp(url, "about:history")) {

        int hist = !strcmp(url, "about:history");
        char path[512], line[4500];

        config_path(hist ? "browser-history" : "bookmarks", path, sizeof(path));
        ADD("<title>%s</title><h1>%s</h1><ul>", hist ? "History" : "Bookmarks", hist ? "History" : "Bookmarks");

        FILE *f = fopen(path, "r");
        char *lines[200];
        int n = 0;

        while (f && fgets(line, sizeof(line), f)) {

            line[strcspn(line, "\n")] = 0;

            if (hist && n == 200) {

                free(lines[0]);
                memmove(lines, lines + 1, sizeof(char *) * 199);
                n--;
            }

            if (n < 200)
                lines[n++] = strdup(line);
        }

        if (f)
            fclose(f);

        for (int i = hist ? n - 1 : 0; hist ? i >= 0 : i < n; hist ? i-- : i++) {

            char *tab = lines[i] ? strchr(lines[i], '\t') : NULL;
            char u[2200], title[600];

            if (tab) {

                *tab = 0;
                html_escape(lines[i], u, sizeof(u));
                html_escape(tab[1] ? tab + 1 : lines[i], title, sizeof(title));
                ADD("<li><a href=\"%s\">%s</a> <small>%.80s</small></li>", u, title, u);
            }

            free(lines[i]);
        }

        if (!n)
            ADD("<li>Nothing yet.</li>");

        ADD("</ul><p><a href=\"about:home\">Start page</a></p>");
    }
    else {

        ADD("<title>Bharat Browser</title><center><h1>Bharat Browser</h1>"
            "<p>A web browser made in India, written from scratch.</p>"
            "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\">"
            "<input name=\"q\" size=\"40\" placeholder=\"Search the web\"> <input type=\"submit\" value=\"Search\"></form>"
            "</center><h3>Good places to start</h3><ul>"
            "<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a> - the free encyclopedia</li>"
            "<li><a href=\"https://en.wikipedia.org/wiki/India\">India</a> on Wikipedia</li>"
            "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a> - news, text only</li>"
            "<li><a href=\"https://text.npr.org/\">NPR Text</a> - news, text only</li>"
            "<li><a href=\"https://www.gutenberg.org/\">Project Gutenberg</a> - free books</li>"
            "<li><a href=\"https://github.com/bicky21/Bharat-Linux\">Bharat Linux</a> on GitHub</li></ul>");

        char path[512], line[4500];

        config_path("bookmarks", path, sizeof(path));

        FILE *f = fopen(path, "r");
        int any = 0;

        while (f && fgets(line, sizeof(line), f)) {

            char *tab = strchr(line, '\t');
            char u[2200], title[600];

            line[strcspn(line, "\n")] = 0;

            if (!tab)
                continue;

            *tab = 0;

            if (!any++)
                ADD("<h3>Your bookmarks</h3><ul>");

            html_escape(line, u, sizeof(u));
            html_escape(tab + 1, title, sizeof(title));
            ADD("<li><a href=\"%s\">%s</a></li>", u, title);
        }

        if (f)
            fclose(f);

        if (any)
            ADD("</ul>");

        ADD("<h3>Good to know</h3><ul>"
            "<li>Type an address (wikipedia.org) or words to search in the bar at the top.</li>"
            "<li>Ctrl+T new tab, Ctrl+W close tab, Ctrl+L address bar, Ctrl+D bookmark, Ctrl+F find, "
            "Alt+Left back, Ctrl+R reload, Ctrl+U page source.</li>"
            "<li>Right-click for more: open links in a new tab, copy addresses, download.</li>"
            "<li>No JavaScript is run: pages are lighter and safer, but some sites need it.</li>"
            "<li><a href=\"about:bookmarks\">Bookmarks</a> - <a href=\"about:history\">History</a></li></ul>");
    }

#undef ADD

    show_html(b, t, url, h, len, 1);
    free(h);
}

static void remember(const char *url, const char *title) {

    if (!strncmp(url, "about:", 6))
        return;

    char path[512];

    config_path("browser-history", path, sizeof(path));

    FILE *f = fopen(path, "a");

    if (f) {

        fprintf(f, "%s\t%s\n", url, title && *title ? title : url);
        fclose(f);
    }
}

/* opens url in tab t; add_history: a new step in back/forward */
static void go(struct browser *b, struct tab *t, const char *url, int add_history, const char *post_body) {

    drop_requests(b, t);

    if (add_history) {

        /* everything after the current position is forgotten */
        for (int i = t->at + 1; i < t->nhist; i++)
            free(t->hist[i]);

        t->nhist = t->at + 1;

        if (t->nhist == MAX_HIST) {

            free(t->hist[0]);
            memmove(t->hist, t->hist + 1, sizeof(char *) * (MAX_HIST - 1));
            t->nhist--;
        }

        t->hist[t->nhist] = strdup(url);
        t->at = t->nhist++;
    }

    snprintf(t->url, sizeof(t->url), "%s", url);

    if (t == cur_tab(b)) {

        b->editing = 0;
        snprintf(b->address, sizeof(b->address), "%s", url);
    }

    if (!strncmp(url, "about:", 6)) {

        about_page(b, t, url);
        return;
    }

    t->loading = 1;
    snprintf(b->status, sizeof(b->status), "Opening %.200s ...", url);

    long id;

    if (post_body) {

        /* POST: the length goes on the request line, then the body */
        char extra[32];

        snprintf(extra, sizeof(extra), "%zu", strlen(post_body));
        id = ask(b, t, R_PAGE, 0, "%ld POST %s %s\n", url, extra, post_body);
    }
    else
        id = ask(b, t, R_PAGE, 0, "%ld PAGE %s%s\n", url, NULL, NULL);

    if (id < 0)
        show_error(b, t, url, "The browser's helper (/bin/bharat-fetch) could not be started.");
}

/* ====================================================================
 * answers from the helper
 * ==================================================================== */

static struct request *find_request(struct browser *b, long id) {

    for (int i = 0; i < 256; i++)
        if (b->req[i].used && b->req[i].id == id)
            return &b->req[i];

    return NULL;
}

static int tab_alive(struct browser *b, struct tab *t) {

    for (int i = 0; i < b->ntabs; i++)
        if (b->tabs[i] == t)
            return 1;

    return 0;
}

static void save_download(struct browser *b, const char *url, const char *data, size_t len) {

    const char *home = getenv("HOME");
    char dir[512], path[1200], name[256];
    struct url u;

    snprintf(dir, sizeof(dir), "%s/Downloads", home ? home : "/tmp");
    mkdir(dir, 0755);

    if (url_parse(url, &u) == 0) {

        const char *last = strrchr(u.path, '/');

        snprintf(name, sizeof(name), "%s", last && last[1] ? last + 1 : "download");
        name[strcspn(name, "?")] = 0;
    }
    else
        snprintf(name, sizeof(name), "download");

    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE *f = fopen(path, "w");

    if (f && fwrite(data, 1, len, f) == len && fclose(f) == 0) {

        char msg[1300];

        snprintf(msg, sizeof(msg), "Downloaded to %.900s", path);
        snprintf(b->status, sizeof(b->status), "%.290s", msg);
        dialog_info("Download", msg);
    }
    else {

        if (f)
            fclose(f);

        dialog_info("Download", "The file could not be saved in Downloads.");
    }
}

/* one complete answer */
static void handle(struct browser *b, const char *head, const char *data, size_t len) {

    long id;
    char kind[16];

    if (sscanf(head, "%ld %15s", &id, kind) != 2)
        return;

    struct request *r = find_request(b, id);

    if (!r)
        return;      /* the tab went elsewhere: not wanted any more */

    struct tab *t = r->tab;

    r->used = 0;

    if (!tab_alive(b, t) && r->kind != R_SAVE)
        return;

    if (!strcmp(kind, "ERROR")) {

        const char *msg = strstr(head, "ERROR ") ? strstr(head, "ERROR ") + 6 : "unknown error";

        if (r->kind == R_PAGE)
            show_error(b, t, t->url, msg);
        else if (r->kind == R_IMAGE && r->image < t->page.nimages)
            t->page.images[r->image].state = 3;
        else if (r->kind == R_SAVE)
            dialog_info("Download", msg);

        snprintf(b->status, sizeof(b->status), "%.290s", msg);
        return;
    }

    if (!strcmp(kind, "PAGE")) {

        int status = 0;
        size_t n = 0;
        char rest[4500] = "";

        sscanf(head, "%*d %*s %d %zu %4499[^\n]", &status, &n, rest);

        char *tabc = strchr(rest, '\t');
        const char *ctype = tabc ? tabc + 1 : "text/html";

        if (tabc)
            *tabc = 0;

        if (status >= 300 && status < 400 && len == 0) {

            show_error(b, t, rest, "The page moved, but did not say where.");
            return;
        }

        int html = strcasestr(ctype, "html") != NULL;
        int text_like = !strncasecmp(ctype, "text/", 5) || strcasestr(ctype, "json") || strcasestr(ctype, "xml");

        if (!strncasecmp(ctype, "image/", 6)) {

            /* a picture by itself: a tiny page that shows it */
            char page[5000], u[4400];

            html_escape(rest, u, sizeof(u));
            snprintf(page, sizeof(page), "<title>Picture</title><img src=\"%s\">", u);
            show_html(b, t, rest, page, strlen(page), 1);
        }
        else if (html || text_like) {

            show_html(b, t, rest, data, len, html);

            /* the address changed (a redirect): the history shows the real one */
            if (t->at < t->nhist && t->hist[t->at] && strcmp(t->hist[t->at], rest)) {

                free(t->hist[t->at]);
                t->hist[t->at] = strdup(rest);
            }

            remember(rest, t->page.title);
        }
        else {

            /* a file (PDF, ZIP ...): save it */
            save_download(b, rest, data, len);
            t->loading = 0;

            if (t->at > 0 && t->hist[t->at] && !strcmp(t->hist[t->at], rest)) {

                free(t->hist[t->at]);
                t->nhist = t->at--;
                snprintf(t->url, sizeof(t->url), "%s", t->hist[t->at]);
                snprintf(b->address, sizeof(b->address), "%s", t->url);
            }
        }

        if (status >= 400)
            snprintf(b->status, sizeof(b->status), "The server answered %d", status);
        else
            b->status[0] = 0;

        return;
    }

    if (!strcmp(kind, "IMAGE") && r->image < t->page.nimages) {

        int w = 0, h = 0;

        sscanf(head, "%*d %*s %d %d", &w, &h);

        struct page_image *im = &t->page.images[r->image];
        uint8_t *px = malloc(len ? len : 1);

        if (!px || len != (size_t)w * (size_t)h * 4) {

            free(px);
            im->state = 3;
            return;
        }

        /* the fetcher sends red, green, blue, alpha: our images use the same */
        memcpy(px, data, len);
        free(im->img.px);
        im->img.px = px;
        im->img.w = w;
        im->img.h = h;
        im->state = 2;

        /* sizes may have changed: lay the page out again */
        page_layout(&t->page, t->page.width);
        return;
    }

    if (!strcmp(kind, "SAVED")) {

        char path[4200] = "", msg[4300];

        sscanf(head, "%*d %*s %*u %4199[^\n]", path);
        snprintf(msg, sizeof(msg), "Downloaded to %.4000s", path);
        dialog_info("Download", msg);
    }
}

static void browser_readable_inner(struct window *win);

static void browser_readable(struct window *win) {

    browser_readable_inner(win);
    wm_damage_window(win);
}

static void browser_readable_inner(struct window *win) {

    struct browser *b = win->data;

    while (1) {

        if (b->in_len + 65536 > b->in_cap) {

            size_t cap = b->in_cap ? b->in_cap * 2 : 262144;
            char *bigger = realloc(b->in, cap);

            if (!bigger)
                return;

            b->in = bigger;
            b->in_cap = cap;
        }

        ssize_t n = read(b->from_fetcher, b->in + b->in_len, b->in_cap - b->in_len - 1);

        if (n == 0) {

            /* the helper stopped (it crashed on something): start afresh next time */
            stop_fetcher(b);

            for (int i = 0; i < b->ntabs; i++) {

                if (b->tabs[i]->loading)
                    show_error(b, b->tabs[i], b->tabs[i]->url, "The page could not be read.");

                drop_requests(b, b->tabs[i]);
            }

            return;
        }

        if (n < 0)
            break;

        b->in_len += (size_t)n;
    }

    /* complete answers: a head line, then (maybe) data of a known length */
    while (b->in_len) {

        char *nl = memchr(b->in, '\n', b->in_len);

        if (!nl)
            break;

        size_t head_len = (size_t)(nl - b->in);
        char head[4600];

        snprintf(head, sizeof(head), "%.*s", (int)(head_len < sizeof(head) - 1 ? head_len : sizeof(head) - 1), b->in);

        size_t data_len = 0;
        char kind[16] = "";

        sscanf(head, "%*d %15s", kind);

        if (!strcmp(kind, "PAGE"))
            sscanf(head, "%*d %*s %*d %zu", &data_len);
        else if (!strcmp(kind, "IMAGE"))
            sscanf(head, "%*d %*s %*d %*d %zu", &data_len);

        if (b->in_len < head_len + 1 + data_len)
            break;

        handle(b, head, b->in + head_len + 1, data_len);

        size_t used = head_len + 1 + data_len;

        memmove(b->in, b->in + used, b->in_len - used);
        b->in_len -= used;
    }
}

static int browser_fd(struct window *win) {

    struct browser *b = win->data;

    return b->from_fetcher;
}

/* ====================================================================
 * tabs
 * ==================================================================== */

static struct tab *new_tab(struct browser *b, const char *url) {

    if (b->ntabs >= MAX_TABS)
        return NULL;

    struct tab *t = calloc(1, sizeof(*t));

    if (!t)
        return NULL;

    t->page.focus = -1;
    b->tabs[b->ntabs] = t;
    b->cur = b->ntabs++;
    go(b, t, url, 1, NULL);

    return t;
}

static void close_tab(struct browser *b, int i) {

    struct tab *t = b->tabs[i];

    drop_requests(b, t);
    page_free(&t->page);
    free(t->source);

    for (int k = 0; k < t->nhist; k++)
        free(t->hist[k]);

    free(t);
    memmove(b->tabs + i, b->tabs + i + 1, sizeof(b->tabs[0]) * (size_t)(b->ntabs - i - 1));
    b->ntabs--;

    if (b->ntabs == 0) {

        the_window->want_close = 1;
        return;
    }

    if (b->cur >= b->ntabs)
        b->cur = b->ntabs - 1;

    snprintf(b->address, sizeof(b->address), "%s", cur_tab(b)->url);
}

static void switch_tab(struct browser *b, int i) {

    b->cur = i;
    b->editing = 0;
    snprintf(b->address, sizeof(b->address), "%s", cur_tab(b)->url);
    snprintf(the_window->title, sizeof(the_window->title), "%.50s",
             cur_tab(b)->page.title[0] ? cur_tab(b)->page.title : "Bharat Browser");

    /* the window may have changed size while the tab was hidden */
    if (cur_tab(b)->page.width != content_width(the_window))
        page_layout(&cur_tab(b)->page, content_width(the_window));
}

/* ====================================================================
 * drawing
 * ==================================================================== */

static int tab_w(struct browser *b, int w) {

    int tw = (w - 40) / (b->ntabs ? b->ntabs : 1);

    return tw > 200 ? 200 : tw;
}

static int inside_point(int x, int y, int w, int h) {

    return mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h;
}

static void tool_button(int x, int y, const char *label, int enabled) {

    if (enabled && inside_point(x, y, BTN, BTN - 6))
        fill_round(x, y, BTN, BTN - 6, 6, 0xE6E9ED);

    text(&font_bold, x + (BTN - text_width(&font_bold, label)) / 2, y + 5, label, enabled ? C_TEXT : 0xAFB8C1);
}

static void browser_draw(struct window *win, int x, int y, int w, int h) {

    struct browser *b = win->data;
    struct tab *t = cur_tab(b);

    /* tabs */
    fill(x, y, w, TABS_H, 0xDDE1E6);

    int tw = tab_w(b, w);

    for (int i = 0; i < b->ntabs; i++) {

        int tx = x + 4 + i * tw;
        char label[64], title[300];

        snprintf(title, sizeof(title), "%.290s", b->tabs[i]->loading ? "Loading..." :
                 b->tabs[i]->page.title[0] ? b->tabs[i]->page.title : b->tabs[i]->url);
        fill_round(tx, y + 4, tw - 4, TABS_H - 4, 6, i == b->cur ? C_WHITE : 0xEEF0F3);

        int room = tw - 34;
        size_t n = strlen(title);

        while (n > 0 && text_width(&font_ui, title) > room)
            title[--n] = 0;

        snprintf(label, sizeof(label), "%s", title);
        text(&font_ui, tx + 8, y + 10, label, C_TEXT);
        text(&font_ui, tx + tw - 20, y + 9, "x", 0x8C959F);
    }

    fill_round(x + 4 + b->ntabs * tw, y + 6, 26, TABS_H - 8, 6, 0xEEF0F3);
    text(&font_bold, x + 4 + b->ntabs * tw + 9, y + 9, "+", C_TEXT);

    /* toolbar */
    int ty = y + TABS_H;

    fill(x, ty, w, TOOL_H, C_WHITE);
    fill(x, ty + TOOL_H - 1, w, 1, C_BORDER);

    tool_button(x + 6, ty + 7, "<", t->at > 0);
    tool_button(x + 6 + BTN, ty + 7, ">", t->at + 1 < t->nhist);
    tool_button(x + 6 + 2 * BTN, ty + 7, t->loading ? "X" : "R", 1);
    tool_button(x + 6 + 3 * BTN, ty + 7, "H", 1);

    int ax = x + 12 + 4 * BTN, aw = w - (ax - x) - 50;

    fill_round(ax, ty + 6, aw, TOOL_H - 12, 13, b->editing ? 0xFFFFFF : 0xF1F3F5);

    if (b->editing)
        frame(ax, ty + 6, aw, TOOL_H - 12, C_SAFFRON);

    char shown[4300];

    to_ascii(b->address, shown, sizeof(shown));

    const char *start = shown;

    while (*start && text_width(&font_ui, start) > aw - 30)
        start++;

    /* a lock for encrypted pages */
    int secure = !strncmp(t->url, "https://", 8);

    if (secure && !b->editing)
        text(&font_bold, ax + 10, ty + 12, "*", C_GREEN);

    text(&font_ui, ax + (secure && !b->editing ? 22 : 12), ty + 12, start, b->replace_all ? 0x0969DA : C_TEXT);

    if (b->editing)
        fill(ax + 13 + text_width(&font_ui, start), ty + 11, 1, 18, C_TEXT);

    /* bookmark star */
    tool_button(x + w - 44, ty + 7, "+*", 1);

    /* the page */
    int py = ty + TOOL_H, ph = h - TABS_H - TOOL_H - STATUS_H;

    gfx_clip(x, py, w, ph);
    page_draw(&t->page, x, py, w - 10, ph, t->scroll);

    /* scroll bar */
    if (t->page.height > ph) {

        int bh = ph * ph / t->page.height;
        int by = py + (ph - bh) * t->scroll / (t->page.height - ph > 0 ? t->page.height - ph : 1);

        if (bh < 24) bh = 24;

        fill(x + w - 10, py, 10, ph, 0xF1F3F5);
        fill_round(x + w - 8, by, 6, bh, 3, 0xAFB8C1);
    }

    gfx_noclip();
    gfx_clip(x, y, w, h);

    /* status bar */
    int sy = y + h - STATUS_H;
    char st[400];

    fill(x, sy, w, STATUS_H, 0xF6F8FA);
    fill(x, sy, w, 1, C_BORDER);
    to_ascii(b->hover[0] ? b->hover : (t->loading ? b->status : b->status[0] ? b->status : "Ready"), st, sizeof(st));
    text(&font_ui, x + 8, sy + 3, st, C_MUTED);
}

/* ====================================================================
 * forms
 * ==================================================================== */

static void submit(struct browser *b, struct tab *t, int button) {

    struct page *p = &t->page;
    int form = p->inputs[button].form;

    if (form < 0 || form >= p->nforms)
        return;

    char body[8192] = "";

    for (int i = 0; i < p->ninputs; i++) {

        struct page_input *in = &p->inputs[i];

        if (in->form != form || !in->name[0])
            continue;

        if ((in->kind == IN_CHECKBOX || in->kind == IN_RADIO) && !in->checked)
            continue;

        if (in->kind == IN_SUBMIT && i != button)
            continue;

        const char *value = in->kind == IN_SELECT ? (in->noptions ? in->options[in->chosen] : "") :
                            (in->kind == IN_CHECKBOX || in->kind == IN_RADIO) && !in->value[0] ? "on" : in->value;
        char n[400], v[1600];

        url_encode(in->name, n, sizeof(n));
        url_encode(value, v, sizeof(v));

        size_t used = strlen(body);

        snprintf(body + used, sizeof(body) - used, "%s%s=%s", used ? "&" : "", n, v);
    }

    struct page_form *f = &p->forms[form];

    if (f->post) {

        go(b, t, f->action, 1, body);
        return;
    }

    char url[4200 + 8192];

    snprintf(url, sizeof(url), "%.*s?%s", (int)strcspn(f->action, "?"), f->action, body);
    url[4190] = 0;
    go(b, t, url, 1, NULL);
}

/* ====================================================================
 * menus and actions
 * ==================================================================== */

static int menu_link = -1, menu_image = -1;

static void act_back(void *ctx, int arg);
static void act_forward(void *ctx, int arg);

static void act_reload(void *ctx, int arg) {

    struct browser *b = ctx;

    (void)arg;
    go(b, cur_tab(b), cur_tab(b)->url, 0, NULL);
}

static void act_new_tab_link(void *ctx, int arg) {

    struct browser *b = ctx;
    int keep = b->cur;
    struct page *p = &cur_tab(b)->page;

    (void)arg;

    if (menu_link >= 0 && menu_link < p->nlinks) {

        char *url = strdup(p->links[menu_link]);

        if (url && new_tab(b, url))
            switch_tab(b, keep);     /* opens in the background */

        free(url);
    }
}

static void act_copy_link(void *ctx, int arg) {

    struct browser *b = ctx;
    struct page *p = &cur_tab(b)->page;

    (void)arg;

    if (menu_link >= 0 && menu_link < p->nlinks)
        clip_set_text(p->links[menu_link]);
}

static void act_copy_address(void *ctx, int arg) {

    (void)arg;
    clip_set_text(cur_tab((struct browser *)ctx)->url);
}

static void act_download_link(void *ctx, int arg) {

    struct browser *b = ctx;
    struct page *p = &cur_tab(b)->page;
    const char *url = NULL;

    if (arg == 1 && menu_image >= 0 && menu_image < p->nimages)
        url = p->images[menu_image].url;
    else if (menu_link >= 0 && menu_link < p->nlinks)
        url = p->links[menu_link];

    if (!url)
        return;

    const char *home = getenv("HOME");
    char path[1200], name[256];
    struct url u;

    snprintf(path, sizeof(path), "%s/Downloads", home ? home : "/tmp");
    mkdir(path, 0755);

    if (url_parse(url, &u) == 0) {

        const char *last = strrchr(u.path, '/');

        snprintf(name, sizeof(name), "%s", last && last[1] ? last + 1 : "download");
        name[strcspn(name, "?")] = 0;
    }
    else
        snprintf(name, sizeof(name), "download");

    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/%s", name);

    if (ask(b, cur_tab(b), R_SAVE, 0, "%ld SAVE %s %s\n", url, path, NULL) > 0)
        snprintf(b->status, sizeof(b->status), "Downloading %.250s ...", name);
}

static void act_bookmark(void *ctx, int arg) {

    struct browser *b = ctx;
    struct tab *t = cur_tab(b);
    char path[512];

    (void)arg;

    if (!strncmp(t->url, "about:", 6))
        return;

    config_path("bookmarks", path, sizeof(path));

    FILE *f = fopen(path, "a");

    if (f) {

        fprintf(f, "%s\t%s\n", t->url, t->page.title[0] ? t->page.title : t->url);
        fclose(f);
        snprintf(b->status, sizeof(b->status), "Bookmarked: %.250s", t->page.title[0] ? t->page.title : t->url);
    }
}

static void act_source(void *ctx, int arg) {

    struct browser *b = ctx;
    struct tab *t = cur_tab(b);
    char path[] = "/tmp/page-source-XXXXXX";

    (void)arg;

    int fd = mkstemp(path);

    if (fd < 0 || !t->source) {

        if (fd >= 0)
            close(fd);
        return;
    }

    if (write(fd, t->source, t->source_len) < 0) {
        /* shown empty */
    }

    close(fd);
    open_viewer(path);
    unlink(path);      /* the viewer has read it */
}

static void found(void *ctx, const char *what) {

    struct browser *b = ctx;
    struct tab *t = cur_tab(b);

    snprintf(b->find, sizeof(b->find), "%s", what);

    int y = page_find(&t->page, what, t->scroll + 4);

    if (y < 0)
        y = page_find(&t->page, what, -1);    /* from the top again */

    if (y < 0)
        snprintf(b->status, sizeof(b->status), "\"%.100s\" was not found", what);
    else {

        t->scroll = y - 60 > 0 ? y - 60 : 0;
        snprintf(b->status, sizeof(b->status), "Found \"%.100s\"", what);
    }

    wm_damage_window(the_window);
}

static void act_find(void *ctx, int arg) {

    struct browser *b = ctx;

    (void)arg;
    dialog_ask("Find in page", "Find:", b->find, "Find", the_window, found, b);
}

static void act_new_tab(void *ctx, int arg) { (void)arg; new_tab(ctx, HOME_URL); }
static void act_open_image(void *ctx, int arg) {

    struct browser *b = ctx;
    struct page *p = &cur_tab(b)->page;

    (void)arg;

    if (menu_image >= 0 && menu_image < p->nimages) {

        char *url = strdup(p->images[menu_image].url);

        if (url)
            go(b, cur_tab(b), url, 1, NULL);

        free(url);
    }
}

static void act_back(void *ctx, int arg) {

    struct browser *b = ctx;
    struct tab *t = cur_tab(b);

    (void)arg;

    if (t->at > 0) {

        t->at--;
        go(b, t, t->hist[t->at], 0, NULL);
    }
}

static void act_forward(void *ctx, int arg) {

    struct browser *b = ctx;
    struct tab *t = cur_tab(b);

    (void)arg;

    if (t->at + 1 < t->nhist) {

        t->at++;
        go(b, t, t->hist[t->at], 0, NULL);
    }
}

static void page_menu(struct browser *b, int link, int image) {

    struct tab *t = cur_tab(b);

    menu_link = link;
    menu_image = image;
    popup_begin(b);

    if (link >= 0) {

        popup_add("Open link in new tab", act_new_tab_link, 0, 1);
        popup_add("Copy link address", act_copy_link, 0, 1);
        popup_add("Download link", act_download_link, 0, 1);
        popup_line();
    }

    if (image >= 0) {

        popup_add("Open picture", act_open_image, 0, 1);
        popup_add("Save picture", act_download_link, 1, 1);
        popup_line();
    }

    popup_add("Back", act_back, 0, t->at > 0);
    popup_add("Forward", act_forward, 0, t->at + 1 < t->nhist);
    popup_add("Reload", act_reload, 0, 1);
    popup_line();
    popup_add("Bookmark this page", act_bookmark, 0, strncmp(t->url, "about:", 6) != 0);
    popup_add("Copy page address", act_copy_address, 0, 1);
    popup_add("Find in page...", act_find, 0, 1);
    popup_add("View page source", act_source, 0, t->source != NULL);
    popup_line();
    popup_add("New tab", act_new_tab, 0, 1);
    popup_show();
}

static void choose_option(void *ctx, int arg) {

    struct browser *b = ctx;
    struct page *p = &cur_tab(b)->page;

    if (p->focus >= 0 && p->focus < p->ninputs && arg < p->inputs[p->focus].noptions)
        p->inputs[p->focus].chosen = arg;

    wm_damage_window(the_window);
}

/* ====================================================================
 * mouse and keys
 * ==================================================================== */

static void browser_click(struct window *win, int x, int y, int button, int dbl) {

    struct browser *b = win->data;
    struct tab *t = cur_tab(b);
    int cx, cy, w, h;

    (void)dbl;
    content_box(win, &cx, &cy, &w, &h);

    /* tabs */
    if (y < TABS_H) {

        int tw = tab_w(b, w);
        int i = (x - 4) / tw;

        if (x >= 4 + b->ntabs * tw && x < 4 + b->ntabs * tw + 26) {

            new_tab(b, HOME_URL);
            return;
        }

        if (i >= 0 && i < b->ntabs) {

            if (button == 3 || x >= 4 + i * tw + tw - 26)
                close_tab(b, i);
            else
                switch_tab(b, i);
        }

        return;
    }

    /* toolbar */
    if (y < TABS_H + TOOL_H) {

        int ax = 12 + 4 * BTN;

        b->editing = 0;

        if (x >= 6 && x < 6 + BTN) act_back(b, 0);
        else if (x >= 6 + BTN && x < 6 + 2 * BTN) act_forward(b, 0);
        else if (x >= 6 + 2 * BTN && x < 6 + 3 * BTN) {

            if (t->loading) {

                drop_requests(b, t);
                stop_fetcher(b);
                t->loading = 0;
                snprintf(b->status, sizeof(b->status), "Stopped");
            }
            else
                act_reload(b, 0);
        }
        else if (x >= 6 + 3 * BTN && x < 6 + 4 * BTN) go(b, t, HOME_URL, 1, NULL);
        else if (x >= w - 44) act_bookmark(b, 0);
        else if (x >= ax) {

            b->editing = 1;
            b->replace_all = 1;

            if (button == 2) {

                popup_begin(b);
                popup_add("Copy page address", act_copy_address, 0, 1);
                popup_show();
            }
        }

        return;
    }

    if (y >= h - STATUS_H)
        return;

    /* the page */
    int px = x, py = y - TABS_H - TOOL_H + t->scroll;
    int link = page_link_at(&t->page, px, py);
    int input = page_input_at(&t->page, px, py);
    int image = -1;

    for (int i = 0; i < t->page.nitems; i++) {

        struct item *it = &t->page.items[i];

        if (it->type == IT_IMAGE && px >= it->x && px < it->x + it->w && py >= it->y && py < it->y + it->h)
            image = it->image;
    }

    b->editing = 0;

    if (button == 2) {

        page_menu(b, link, image);
        return;
    }

    t->page.focus = -1;

    if (input >= 0) {

        struct page_input *in = &t->page.inputs[input];

        if (in->kind == IN_SUBMIT) {

            submit(b, t, input);
            return;
        }

        if (in->kind == IN_CHECKBOX) {

            in->checked = !in->checked;
            return;
        }

        if (in->kind == IN_RADIO) {

            for (int i = 0; i < t->page.ninputs; i++)
                if (t->page.inputs[i].kind == IN_RADIO && t->page.inputs[i].form == in->form &&
                    !strcmp(t->page.inputs[i].name, in->name))
                    t->page.inputs[i].checked = 0;

            in->checked = 1;
            return;
        }

        t->page.focus = input;

        if (in->kind == IN_SELECT) {

            popup_begin(b);

            for (int i = 0; i < in->noptions && i < 16; i++) {

                char o[48];

                to_ascii(in->option_text[i], o, sizeof(o));
                popup_add(o, choose_option, i, 1);
            }

            popup_show();
        }

        return;
    }

    if (link >= 0) {

        char *url = strdup(t->page.links[link]);

        if (!url)
            return;

        if (button == 3)
            new_tab(b, url);
        else
            go(b, t, url, 1, NULL);

        free(url);
    }
}

static void scroll_by(struct browser *b, int dy) {

    struct tab *t = cur_tab(b);
    int cx, cy, w, h;

    content_box(the_window, &cx, &cy, &w, &h);

    int max = t->page.height - (h - TABS_H - TOOL_H - STATUS_H);

    t->scroll += dy;

    if (t->scroll > max) t->scroll = max;
    if (t->scroll < 0) t->scroll = 0;
}

static void browser_wheel(struct window *win, int value) {

    scroll_by(win->data, -value * 60);
}

static void type_into(char *value, size_t size, struct event *ev) {

    size_t n = strlen(value);

    if (ev->key == '\b') {

        if (n)
            value[n - 1] = 0;
    }
    else if ((ev->mods & MOD_CTRL) && (ev->key == 'v' || ev->key == 'V')) {

        if (clip_text())
            snprintf(value + n, size - n, "%s", clip_text());

        /* one line only */
        value[strcspn(value, "\r\n")] = 0;
    }
    else if (ev->key >= 32 && ev->key < 127 && n + 1 < size) {

        value[n] = (char)ev->key;
        value[n + 1] = 0;
    }
}

static void browser_key_inner(struct window *win, struct event *ev);

/* keys: then draw again (the window manager leaves that to windows that
   read from a file, like this one) */
static void browser_key(struct window *win, struct event *ev) {

    browser_key_inner(win, ev);

    if (wm_window_alive(win))
        wm_damage_window(win);
}

static void browser_key_inner(struct window *win, struct event *ev) {

    struct browser *b = win->data;
    struct tab *t = cur_tab(b);
    int ctrl = ev->mods & MOD_CTRL, k = ev->key;
    int cx, cy, w, h;

    content_box(win, &cx, &cy, &w, &h);

    int page_h = h - TABS_H - TOOL_H - STATUS_H;

    /* shortcuts that work everywhere */
    if (ctrl && (k == 't' || k == 'T')) { new_tab(b, HOME_URL); b->editing = 1; b->replace_all = 1; return; }
    if (ctrl && (k == 'w' || k == 'W')) { close_tab(b, b->cur); return; }
    if (ctrl && (k == 'l' || k == 'L')) { b->editing = 1; b->replace_all = 1; return; }
    if (ctrl && (k == 'r' || k == 'R')) { act_reload(b, 0); return; }
    if (ctrl && (k == 'd' || k == 'D')) { act_bookmark(b, 0); return; }
    if (ctrl && (k == 'f' || k == 'F')) { act_find(b, 0); return; }
    if (ctrl && (k == 'u' || k == 'U')) { act_source(b, 0); return; }
    if (ctrl && k == '\t') { switch_tab(b, (b->cur + 1) % b->ntabs); return; }
    if ((ev->mods & MOD_ALT) && k == K_LEFT) { act_back(b, 0); return; }
    if ((ev->mods & MOD_ALT) && k == K_RIGHT) { act_forward(b, 0); return; }

    if (b->editing) {

        if (k == '\r') {

            char url[4300];

            address_to_url(b->address, url, sizeof(url));
            b->editing = 0;
            go(b, t, url, 1, NULL);
        }
        else if (k == 27) {

            b->editing = 0;
            snprintf(b->address, sizeof(b->address), "%s", t->url);
        }
        else {

            if (b->replace_all && (k == '\b' || (k >= 32 && k < 127) || (ctrl && (k == 'v' || k == 'V'))))
                b->address[0] = 0;

            b->replace_all = 0;
            type_into(b->address, sizeof(b->address), ev);
        }

        return;
    }

    /* typing into a form field on the page */
    if (t->page.focus >= 0 && t->page.focus < t->page.ninputs) {

        struct page_input *in = &t->page.inputs[t->page.focus];

        if (k == '\r' && in->kind != IN_TEXTAREA) {

            /* Enter sends the form: with its first button, if it has one */
            int button = t->page.focus;

            for (int i = 0; i < t->page.ninputs; i++)
                if (t->page.inputs[i].form == in->form && t->page.inputs[i].kind == IN_SUBMIT) {

                    button = i;
                    break;
                }

            submit(b, t, button);
            return;
        }

        if (k == '\t') {

            for (int i = t->page.focus + 1; i < t->page.ninputs; i++)
                if (t->page.inputs[i].kind == IN_TEXT || t->page.inputs[i].kind == IN_PASSWORD ||
                    t->page.inputs[i].kind == IN_TEXTAREA) {

                    t->page.focus = i;
                    return;
                }

            t->page.focus = -1;
            return;
        }

        if (k == 27) {

            t->page.focus = -1;
            return;
        }

        if (in->kind == IN_TEXT || in->kind == IN_PASSWORD || in->kind == IN_TEXTAREA) {

            if (k == '\r')
                k = ' ';

            type_into(in->value, sizeof(in->value), ev);
            return;
        }
    }

    switch (k) {

        case K_DOWN:  scroll_by(b, 40); break;
        case K_UP:    scroll_by(b, -40); break;
        case K_PGDN:
        case ' ':     scroll_by(b, page_h - 40); break;
        case K_PGUP:  scroll_by(b, -(page_h - 40)); break;
        case K_HOME:  cur_tab(b)->scroll = 0; break;
        case K_END:   scroll_by(b, 1 << 28); break;
        case '\b':    act_back(b, 0); break;
    }
}

static void browser_move(struct window *win, int x, int y) {

    struct browser *b = win->data;
    struct tab *t = cur_tab(b);
    char old[300];

    snprintf(old, sizeof(old), "%s", b->hover);
    b->hover[0] = 0;

    if (y > TABS_H + TOOL_H) {

        int link = page_link_at(&t->page, x, y - TABS_H - TOOL_H + t->scroll);

        if (link >= 0)
            snprintf(b->hover, sizeof(b->hover), "%.290s", t->page.links[link]);
    }

    if (strcmp(old, b->hover))
        wm_damage_window(win);
}

static void browser_resized(struct window *win) {

    struct browser *b = win->data;

    for (int i = 0; i < b->ntabs; i++)
        if (i == b->cur)
            page_layout(&b->tabs[i]->page, content_width(win));
}

static void browser_destroy(struct window *win) {

    struct browser *b = win->data;

    while (b->ntabs) {

        b->ntabs--;

        struct tab *t = b->tabs[b->ntabs];

        page_free(&t->page);
        free(t->source);

        for (int k = 0; k < t->nhist; k++)
            free(t->hist[k]);

        free(t);
    }

    stop_fetcher(b);
    free(b->in);
    free(b);
    the_window = NULL;
}

void open_browser(const char *url) {

    if (the_window) {

        /* one browser window: a new tab in it */
        new_tab(the_window->data, url ? url : HOME_URL);
        wm_redraw();
        return;
    }

    struct window *win = wm_new_window("Bharat Browser", 900, 640);

    if (!win)
        return;

    struct browser *b = calloc(1, sizeof(*b));

    if (!b) {

        win->want_close = 1;
        return;
    }

    the_window = win;
    b->pid = -1;
    b->to_fetcher = b->from_fetcher = -1;
    win->data = b;
    win->draw = browser_draw;
    win->click = browser_click;
    win->key = browser_key;
    win->wheel = browser_wheel;
    win->fd = browser_fd;
    win->readable = browser_readable;
    win->resized = browser_resized;
    win->destroy = browser_destroy;
    win->hover = browser_move;
    win->min_w = 480;
    win->min_h = 300;

    new_tab(b, url ? url : HOME_URL);
}
