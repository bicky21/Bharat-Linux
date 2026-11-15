/*
 * html.c - reads a web page (HTML) into a tree of elements and text.
 *
 * Web pages are often not written correctly, so the reader is forgiving:
 * it closes paragraphs and list items by itself, ignores stray end tags,
 * and never fails. <script> and <style> are kept out of the tree (no
 * JavaScript; styles come from the browser's own simple rules).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "html.h"

static struct node *new_node(int type, struct node *parent) {

    struct node *n = calloc(1, sizeof(*n));

    if (!n)
        return NULL;

    n->type = type;
    n->parent = parent;

    if (parent) {

        if (parent->last)
            parent->last->next = n;
        else
            parent->first = n;

        parent->last = n;
    }

    return n;
}

void html_free(struct node *n) {

    while (n) {

        struct node *next = n->next;

        html_free(n->first);

        for (int i = 0; i < n->nattrs; i++) {

            free(n->attrs[i].name);
            free(n->attrs[i].value);
        }

        free(n->attrs);
        free(n->text);
        free(n);
        n = next;
    }
}

const char *html_attr(const struct node *n, const char *name) {

    for (int i = 0; n && i < n->nattrs; i++)
        if (!strcasecmp(n->attrs[i].name, name))
            return n->attrs[i].value;

    return NULL;
}

/* ---------- characters: &amp; and friends, UTF-8 ---------- */

static void put_utf8(char **out, unsigned cp) {

    char *o = *out;

    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }

    *out = o;
}

static const struct { const char *name; unsigned cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"bull", 0x2022}, {"middot", 0xB7}, {"rarr", 0x2192}, {"larr", 0x2190}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"times", 0xD7}, {"divide", 0xF7}, {"euro", 0x20AC}, {"pound", 0xA3},
    {"deg", 0xB0}, {"plusmn", 0xB1}, {"frac12", 0xBD}, {"para", 0xB6}, {"sect", 0xA7},
    {"shy", 0xAD}, {"zwj", 0x200D}, {"zwnj", 0x200C}, {"thinsp", 0x2009}, {"ensp", 0x2002},
    {"emsp", 0x2003}, {"eacute", 0xE9}, {"uarr", 0x2191}, {"darr", 0x2193}, {"hearts", 0x2665},
    {NULL, 0}
};

/* decodes the entities of s into a new string */
static char *decode(const char *s, size_t len) {

    char *out = malloc(len * 4 + 1), *o = out;

    if (!out)
        return NULL;

    for (size_t i = 0; i < len; i++) {

        if (s[i] != '&') {

            *o++ = s[i];
            continue;
        }

        size_t j = i + 1;

        while (j < len && j - i < 12 && (isalnum((unsigned char)s[j]) || s[j] == '#'))
            j++;

        unsigned cp = 0;
        int ok = 0;

        if (j > i + 1 && s[i + 1] == '#') {

            cp = (s[i + 2] == 'x' || s[i + 2] == 'X') ? (unsigned)strtoul(s + i + 3, NULL, 16)
                                                       : (unsigned)strtoul(s + i + 2, NULL, 10);
            ok = cp > 0 && cp < 0x110000;
        }
        else {

            for (int k = 0; entities[k].name; k++) {

                size_t n = strlen(entities[k].name);

                if (j - i - 1 == n && !strncmp(s + i + 1, entities[k].name, n)) {

                    cp = entities[k].cp;
                    ok = 1;
                    break;
                }
            }
        }

        if (!ok) {

            *o++ = '&';
            continue;
        }

        put_utf8(&o, cp);
        i = (j < len && s[j] == ';') ? j : j - 1;
    }

    *o = 0;

    return out;
}

/* ---------- which tags close which ---------- */

static int is_void(const char *t) {

    static const char *v[] = {"br", "img", "hr", "input", "meta", "link", "area", "base", "col",
                              "embed", "param", "source", "track", "wbr", NULL};

    for (int i = 0; v[i]; i++)
        if (!strcmp(t, v[i]))
            return 1;

    return 0;
}

static int is_block_breaker(const char *t) {

    static const char *b[] = {"p", "div", "ul", "ol", "table", "h1", "h2", "h3", "h4", "h5", "h6",
                              "pre", "blockquote", "form", "hr", "section", "article", "header",
                              "footer", "nav", "dl", "figure", "main", "aside", NULL};

    for (int i = 0; b[i]; i++)
        if (!strcmp(t, b[i]))
            return 1;

    return 0;
}

/* an open <p> ends when a block starts; <li> when the next <li> starts ... */
static struct node *implied_close(struct node *cur, const char *tag) {

    for (struct node *n = cur; n && n->parent; n = n->parent) {

        if (!strcmp(n->tag, "p") && is_block_breaker(tag))
            return n->parent;

        if (!strcmp(n->tag, tag) && (!strcmp(tag, "li") || !strcmp(tag, "option") || !strcmp(tag, "dt") ||
                                     !strcmp(tag, "dd") || !strcmp(tag, "tr") || !strcmp(tag, "td") ||
                                     !strcmp(tag, "th")))
            return n->parent;

        if ((!strcmp(tag, "td") || !strcmp(tag, "th")) && !strcmp(n->tag, "tr"))
            break;

        if (!strcmp(tag, "tr") && (!strcmp(n->tag, "table") || !strcmp(n->tag, "tbody")))
            break;

        if (!strcmp(tag, "li") && (!strcmp(n->tag, "ul") || !strcmp(n->tag, "ol")))
            break;

        /* the search stops at the nearest block that could hold it */
        if (!strcmp(n->tag, "div") || !strcmp(n->tag, "table") || !strcmp(n->tag, "body"))
            break;
    }

    return cur;
}

/* ---------- the reader ---------- */

struct node *html_parse(const char *src, size_t len, char *title, size_t title_size) {

    struct node *root = new_node(N_ELEM, NULL), *cur = root;
    size_t i = 0;

    if (!root)
        return NULL;

    snprintf(root->tag, sizeof(root->tag), "document");

    if (title)
        title[0] = 0;

    while (i < len) {

        if (src[i] != '<') {

            size_t j = i;

            while (j < len && src[j] != '<')
                j++;

            struct node *t = new_node(N_TEXT, cur);

            if (t)
                t->text = decode(src + i, j - i);

            i = j;
            continue;
        }

        /* <!-- comments --> */
        if (i + 3 < len && !strncmp(src + i, "<!--", 4)) {

            const char *end = memmem(src + i + 4, len - i - 4, "-->", 3);

            i = end ? (size_t)(end - src) + 3 : len;
            continue;
        }

        /* <!DOCTYPE ...>, <?xml ...?> */
        if (i + 1 < len && (src[i + 1] == '!' || src[i + 1] == '?')) {

            while (i < len && src[i] != '>')
                i++;

            i++;
            continue;
        }

        int closing = i + 1 < len && src[i + 1] == '/';
        size_t j = i + 1 + closing;
        char tag[16] = "";
        size_t k = 0;

        while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '-' || src[j] == ':')) {

            if (k + 1 < sizeof(tag))
                tag[k++] = (char)tolower((unsigned char)src[j]);

            j++;
        }

        tag[k] = 0;

        if (!tag[0]) {

            /* a lone "<" in the text */
            struct node *t = new_node(N_TEXT, cur);

            if (t)
                t->text = strdup("<");

            i++;
            continue;
        }

        /* attributes */
        struct attr attrs[32];
        int nattrs = 0, self_close = 0;

        while (j < len && src[j] != '>') {

            while (j < len && (isspace((unsigned char)src[j]) || src[j] == '/')) {

                if (src[j] == '/')
                    self_close = 1;

                j++;
            }

            if (j >= len || src[j] == '>')
                break;

            size_t ns = j;

            while (j < len && !isspace((unsigned char)src[j]) && src[j] != '=' && src[j] != '>' && src[j] != '/')
                j++;

            size_t ne = j, vs = j, ve = j;

            while (j < len && isspace((unsigned char)src[j]))
                j++;

            if (j < len && src[j] == '=') {

                j++;

                while (j < len && isspace((unsigned char)src[j]))
                    j++;

                if (j < len && (src[j] == '"' || src[j] == '\'')) {

                    char q = src[j++];

                    vs = j;

                    while (j < len && src[j] != q)
                        j++;

                    ve = j;

                    if (j < len)
                        j++;
                }
                else {

                    vs = j;

                    while (j < len && !isspace((unsigned char)src[j]) && src[j] != '>')
                        j++;

                    ve = j;
                }
            }

            self_close = 0;

            if (ne > ns && nattrs < 32 && !closing) {

                attrs[nattrs].name = strndup(src + ns, ne - ns);
                attrs[nattrs].value = decode(src + vs, ve - vs);

                for (char *c = attrs[nattrs].name; c && *c; c++)
                    *c = (char)tolower((unsigned char)*c);

                nattrs++;
            }
        }

        i = j < len ? j + 1 : len;

        if (closing) {

            /* close the nearest open element with this name, if any */
            for (struct node *n = cur; n && n->parent; n = n->parent) {

                if (!strcmp(n->tag, tag)) {

                    cur = n->parent;
                    break;
                }
            }

            continue;
        }

        /* <script>, <style>: skip their contents (keep <title> text) */
        if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "title") ||
            !strcmp(tag, "textarea") || !strcmp(tag, "noscript") || !strcmp(tag, "template")) {

            char end[24];

            snprintf(end, sizeof(end), "</%s", tag);

            const char *stop = strcasestr(src + i, end);
            size_t endpos = stop && (size_t)(stop - src) <= len ? (size_t)(stop - src) : len;

            if (!strcmp(tag, "title") && title) {

                char *t = decode(src + i, endpos - i);

                if (t) {

                    snprintf(title, title_size, "%s", t);
                    free(t);
                }
            }
            else if (!strcmp(tag, "textarea") || !strcmp(tag, "noscript")) {

                struct node *n = new_node(N_ELEM, cur);

                if (n) {

                    snprintf(n->tag, sizeof(n->tag), "%s", tag);
                    n->attrs = malloc(sizeof(struct attr) * (size_t)(nattrs ? nattrs : 1));
                    memcpy(n->attrs, attrs, sizeof(struct attr) * (size_t)nattrs);
                    n->nattrs = nattrs;
                    nattrs = 0;

                    /* noscript: we have no JavaScript, so its content is for us */
                    if (!strcmp(tag, "noscript")) {

                        struct node *inner = html_parse(src + i, endpos - i, NULL, 0);

                        if (inner) {

                            n->first = inner->first;
                            n->last = inner->last;

                            for (struct node *c = n->first; c; c = c->next)
                                c->parent = n;

                            inner->first = inner->last = NULL;
                            html_free(inner);
                        }
                    }
                    else {

                        struct node *t = new_node(N_TEXT, n);

                        if (t)
                            t->text = decode(src + i, endpos - i);
                    }
                }
            }

            for (int a = 0; a < nattrs; a++) {

                free(attrs[a].name);
                free(attrs[a].value);
            }

            i = endpos;

            while (i < len && src[i] != '>')
                i++;

            i++;
            continue;
        }

        cur = implied_close(cur, tag);

        struct node *n = new_node(N_ELEM, cur);

        if (!n)
            break;

        snprintf(n->tag, sizeof(n->tag), "%s", tag);

        if (nattrs) {

            n->attrs = malloc(sizeof(struct attr) * (size_t)nattrs);

            if (n->attrs) {

                memcpy(n->attrs, attrs, sizeof(struct attr) * (size_t)nattrs);
                n->nattrs = nattrs;
            }
        }

        if (!is_void(tag) && !self_close)
            cur = n;
    }

    return root;
}

/* the first element with this tag, anywhere below n */
struct node *html_find(struct node *n, const char *tag) {

    for (; n; n = n->next) {

        if (n->type == N_ELEM && !strcmp(n->tag, tag))
            return n;

        struct node *inside = html_find(n->first, tag);

        if (inside)
            return inside;
    }

    return NULL;
}
