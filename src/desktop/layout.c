/*
 * layout.c - puts a web page on the screen: words into lines, lines into
 * blocks, lists, tables, pictures and form fields.
 *
 * The browser's own simple rules decide how things look (headings big,
 * links blue, lists indented ...). Pages' style sheets (CSS) are not
 * read yet, except "display: none" (things the page hides).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "layout.h"
#include "../liburl.h"

#define url_join url_resolve

#define C_LINK   0x0969DA
#define C_PAGE   0x1F2328
#define C_QUIET  0x57606A
#define MARGIN   16

/* ====================================================================
 * text: UTF-8 to what the fonts can draw (ASCII for now)
 * ==================================================================== */

void to_ascii(const char *in, char *out, size_t size) {

    size_t o = 0;
    const unsigned char *s = (const unsigned char *)in;

    while (*s && o + 4 < size) {

        unsigned cp;
        int n;

        if (*s < 0x80) { cp = *s; n = 1; }
        else if ((*s & 0xE0) == 0xC0 && s[1]) { cp = ((unsigned)(*s & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
        else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) { cp = ((unsigned)(*s & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
        else if ((*s & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { cp = 0x10000; n = 4; }
        else { cp = '?'; n = 1; }

        s += n;

        const char *rep = NULL;

        switch (cp) {
            case 0xA0: case 0x2002: case 0x2003: case 0x2009: rep = " "; break;
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: rep = "'"; break;
            case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: rep = "\""; break;
            case 0x2013: case 0x2014: case 0x2212: rep = "-"; break;
            case 0x2026: rep = "..."; break;
            case 0x2022: case 0xB7: case 0x25CF: rep = "*"; break;
            case 0x2192: rep = "->"; break;
            case 0x2190: rep = "<-"; break;
            case 0xA9: rep = "(c)"; break;
            case 0xAE: rep = "(R)"; break;
            case 0x2122: rep = "TM"; break;
            case 0xD7: rep = "x"; break;
            case 0xB0: rep = " deg"; break;
            case 0x20B9: rep = "Rs."; break;
            case 0x20AC: rep = "EUR"; break;
            case 0xA3: rep = "GBP"; break;
            case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: rep = ""; break;
            case 0xE9: case 0xE8: case 0xEA: rep = "e"; break;
            case 0xE1: case 0xE0: case 0xE2: case 0xE4: rep = "a"; break;
            case 0xF3: case 0xF6: case 0xF4: rep = "o"; break;
            case 0xFC: case 0xFA: rep = "u"; break;
            case 0xED: rep = "i"; break;
            case 0xF1: rep = "n"; break;
            case 0xE7: rep = "c"; break;
        }

        if (rep) {

            for (; *rep && o + 1 < size; rep++)
                out[o++] = *rep;
        }
        else if (cp < 0x80) {

            out[o++] = cp == '\t' ? ' ' : (char)cp;
        }
        else {

            /* scripts our fonts do not have yet (Hindi, Bengali ...): one mark per run */
            if (!o || out[o - 1] != '?')
                out[o++] = '?';
        }
    }

    out[o] = 0;
}

/* ====================================================================
 * building the lists
 * ==================================================================== */

static struct item *add_item(struct page *p) {

    if (p->nitems == p->cap) {

        int cap = p->cap ? p->cap * 2 : 512;
        struct item *bigger = realloc(p->items, sizeof(struct item) * (size_t)cap);

        if (!bigger)
            return NULL;

        p->items = bigger;
        p->cap = cap;
    }

    struct item *it = &p->items[p->nitems++];

    memset(it, 0, sizeof(*it));
    it->link = -1;
    it->image = -1;
    it->input = -1;

    return it;
}

static int add_link(struct page *p, const char *href) {

    char full[4200];

    if (!href)
        return -1;

    if (!strncmp(href, "about:", 6))
        snprintf(full, sizeof(full), "%.100s", href);         /* the browser's own pages */
    else if (url_join(p->base, href, full, sizeof(full)) != 0)
        return -1;

    if (p->nlinks == p->link_cap) {

        int cap = p->link_cap ? p->link_cap * 2 : 128;
        char **bigger = realloc(p->links, sizeof(char *) * (size_t)cap);

        if (!bigger)
            return -1;

        p->links = bigger;
        p->link_cap = cap;
    }

    p->links[p->nlinks] = strdup(full);

    return p->links[p->nlinks] ? p->nlinks++ : -1;
}

/* ====================================================================
 * the flow of a block: lines of words
 * ==================================================================== */

struct style {
    struct font *font;
    uint32_t color;
    int underline;
    int link;
    int pre;
    int center;
    int form;
};

struct flow {
    struct page *p;
    int left, right;         /* the block's edges */
    int x, y;                /* where the next thing goes */
    int line_start;          /* first item of the current line */
    int line_h, line_asc;
    int center;
    int space;               /* a space is due before the next word */
    int margin;              /* bottom margin already given */
};

static void end_line(struct flow *f) {

    struct page *p = f->p;

    if (f->line_start < p->nitems) {

        /* line the words up on a common base line */
        int width = 0;

        for (int i = f->line_start; i < p->nitems; i++) {

            struct item *it = &p->items[i];

            it->y = f->y + f->line_asc - it->ascent;

            if (it->x + it->w - f->left > width)
                width = it->x + it->w - f->left;
        }

        if (f->center && width < f->right - f->left)
            for (int i = f->line_start; i < p->nitems; i++)
                p->items[i].x += (f->right - f->left - width) / 2;

        f->y += f->line_h + 3;
        f->margin = 0;
    }

    f->x = f->left;
    f->line_h = f->line_asc = 0;
    f->line_start = p->nitems;
    f->space = 0;
}

/* a box on the current line; wraps to the next line when it does not fit */
static struct item *place(struct flow *f, int w, int h, int ascent) {

    if (f->space && f->x > f->left)
        f->x += 4;

    f->space = 0;

    if (f->x + w > f->right && f->x > f->left)
        end_line(f);

    struct item *it = add_item(f->p);

    if (!it)
        return NULL;

    it->x = f->x;
    it->w = w;
    it->h = h;
    it->ascent = ascent;
    f->x += w;

    if (ascent > f->line_asc) {

        f->line_h += ascent - f->line_asc;
        f->line_asc = ascent;
    }

    if (h - ascent > f->line_h - f->line_asc)
        f->line_h = f->line_asc + (h - ascent);

    return it;
}

static void block_gap(struct flow *f, int gap) {

    end_line(f);

    if (gap > f->margin) {

        f->y += gap - f->margin;
        f->margin = gap;
    }
}

static void put_text(struct flow *f, const char *word, struct style *s) {

    int prev = f->p->nitems - 1;
    struct item *it = place(f, text_width(s->font, word), s->font->height, s->font->ascent);

    if (!it)
        return;

    /* a link of several words: one unbroken line under it (and one click area) */
    struct item *pv = prev >= f->line_start && prev >= 0 ? &f->p->items[prev] : NULL;

    if (pv && pv->type == IT_TEXT && s->underline && pv->underline && pv->link == s->link &&
        pv->color == s->color && it->x > pv->x + pv->w && it->x - (pv->x + pv->w) < 12)
        pv->w = it->x - pv->x;

    it->type = IT_TEXT;
    it->font = s->font;
    it->color = s->color;
    it->underline = s->underline;
    it->link = s->link;
    it->text = strdup(word);
}

static void flow_text(struct flow *f, const char *utf8, struct style *s) {

    char *t = malloc(strlen(utf8) * 3 + 8);

    if (!t)
        return;

    to_ascii(utf8, t, strlen(utf8) * 3 + 8);

    if (s->pre) {

        /* keep spaces and line ends as they are */
        char *line = t;

        while (line) {

            char *nl = strchr(line, '\n');

            if (nl)
                *nl = 0;

            if (*line) {

                struct item *it = place(f, text_width(s->font, line), s->font->height, s->font->ascent);

                if (it) {

                    it->type = IT_TEXT;
                    it->font = s->font;
                    it->color = s->color;
                    it->link = s->link;
                    it->underline = s->underline;
                    it->text = strdup(line);
                }
            }

            if (nl) {

                if (f->line_h == 0) {

                    f->line_h = s->font->height;
                    f->line_asc = s->font->ascent;
                }

                end_line(f);
                line = nl + 1;
            }
            else
                line = NULL;
        }

        free(t);
        return;
    }

    char *p = t;

    if (isspace((unsigned char)*p))
        f->space = 1;

    while (*p) {

        while (isspace((unsigned char)*p))
            p++;

        if (!*p)
            break;

        char *end = p;

        while (*end && !isspace((unsigned char)*end))
            end++;

        int trailing = *end != 0;

        *end = 0;
        put_text(f, p, s);
        f->space = trailing;
        p = trailing ? end + 1 : end;

        if (trailing)
            f->space = 1;
    }

    free(t);
}

/* ====================================================================
 * elements
 * ==================================================================== */

static int is_hidden(struct node *n) {

    static const char *skip[] = {"head", "script", "style", "svg", "canvas", "iframe", "video", "audio",
                                 "object", "embed", "template", "map", "math", "datalist", NULL};

    for (int i = 0; skip[i]; i++)
        if (!strcmp(n->tag, skip[i]))
            return 1;

    if (html_attr(n, "hidden"))
        return 1;

    const char *st = html_attr(n, "style");

    if (st) {

        char compact[256];
        size_t k = 0;

        for (const char *c = st; *c && k + 1 < sizeof(compact); c++)
            if (!isspace((unsigned char)*c))
                compact[k++] = (char)tolower((unsigned char)*c);

        compact[k] = 0;

        if (strstr(compact, "display:none") || strstr(compact, "visibility:hidden"))
            return 1;
    }

    const char *type = html_attr(n, "type");

    if (!strcmp(n->tag, "input") && type && !strcasecmp(type, "hidden"))
        return 1;

    return 0;
}

static void flow_children(struct flow *f, struct node *n, struct style *s);
static void flow_node(struct flow *f, struct node *n, struct style *s);

static int is_block(const char *t) {

    static const char *b[] = {"p", "div", "section", "article", "header", "footer", "nav", "main",
                              "aside", "form", "fieldset", "figure", "figcaption", "address", "details",
                              "summary", "center", "body", "html", "dl", "dt", "caption", "legend",
                              "noscript", "tbody", "thead", "tfoot", NULL};

    for (int i = 0; b[i]; i++)
        if (!strcmp(t, b[i]))
            return 1;

    return 0;
}

static int heading_level(const char *t) {

    return t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2] ? t[1] - '0' : 0;
}

static void flow_image(struct flow *f, struct node *n, struct style *s) {

    struct page *p = f->p;

    if (!n->ref) {

        /* no picture: its description instead */
        const char *alt = html_attr(n, "alt");

        if (alt && *alt) {

            struct style q = *s;

            q.color = C_QUIET;
            flow_text(f, "[", &q);
            flow_text(f, alt, &q);
            flow_text(f, "]", &q);
        }

        return;
    }

    struct page_image *im = &p->images[n->ref - 1];
    int w = im->state == 2 ? im->img.w : (im->want_w ? im->want_w : 0);
    int h = im->state == 2 ? im->img.h : (im->want_h ? im->want_h : 0);

    if (im->state != 2 && (!w || !h)) {

        /* not here yet and no size given: a small placeholder */
        w = w ? w : 20;
        h = h ? h : 20;
    }

    int room = f->right - f->left;

    if (w > room) {

        h = (int)((long long)h * room / w);
        w = room;
    }

    struct item *it = place(f, w, h, h);

    if (it) {

        it->type = IT_IMAGE;
        it->image = n->ref - 1;
        it->link = s->link;
    }
}

static void flow_input(struct flow *f, struct node *n, struct style *s) {

    if (!n->ref)
        return;

    struct page_input *in = &f->p->inputs[n->ref - 1];
    int w = 200, h = 26;
    const char *size = html_attr(n, "size");

    switch (in->kind) {

        case IN_SUBMIT: w = text_width(&font_ui, in->label) + 24; break;
        case IN_CHECKBOX:
        case IN_RADIO:  w = 16; h = 16; break;
        case IN_TEXTAREA: w = f->right - f->left > 420 ? 420 : f->right - f->left; h = 80; break;
        case IN_SELECT: {

            int widest = 60;

            for (int i = 0; i < in->noptions; i++) {

                char t[256];

                to_ascii(in->option_text[i], t, sizeof(t));

                int tw = text_width(&font_ui, t);

                if (tw > widest)
                    widest = tw;
            }

            w = widest + 34;
            break;
        }
        default:
            if (size && atoi(size) > 0)
                w = atoi(size) * 8 + 16;
    }

    if (w > f->right - f->left)
        w = f->right - f->left;

    if (w < 16)
        w = 16;

    f->space = 1;

    struct item *it = place(f, w, h, h - 6);

    if (it) {

        it->type = IT_INPUT;
        it->input = n->ref - 1;
        it->link = -1;
    }

    f->space = 1;
    (void)s;
}

static void flow_table(struct flow *f, struct node *table, struct style *s) {

    /* the rows, also inside thead / tbody / tfoot */
    struct node *rows[400];
    int nrows = 0, cols = 1;

    for (struct node *c = table->first; c && nrows < 400; c = c->next) {

        if (c->type != N_ELEM)
            continue;

        if (!strcmp(c->tag, "tr"))
            rows[nrows++] = c;
        else if (!strcmp(c->tag, "thead") || !strcmp(c->tag, "tbody") || !strcmp(c->tag, "tfoot")) {

            for (struct node *r = c->first; r && nrows < 400; r = r->next)
                if (r->type == N_ELEM && !strcmp(r->tag, "tr"))
                    rows[nrows++] = r;
        }
        else if (!strcmp(c->tag, "caption")) {

            struct style q = *s;

            q.center = 1;
            block_gap(f, 4);
            f->center = 1;
            flow_children(f, c, &q);
            end_line(f);
            f->center = s->center;
        }
    }

    for (int r = 0; r < nrows; r++) {

        int n = 0;

        for (struct node *c = rows[r]->first; c; c = c->next) {

            if (c->type == N_ELEM && (!strcmp(c->tag, "td") || !strcmp(c->tag, "th"))) {

                const char *span = html_attr(c, "colspan");

                n += span && atoi(span) > 1 && atoi(span) < 20 ? atoi(span) : 1;
            }
        }

        if (n > cols)
            cols = n;
    }

    block_gap(f, 6);

    int width = f->right - f->left;
    int border = html_attr(table, "border") && atoi(html_attr(table, "border")) > 0;

    /* a "wikitable"-like look when the page asks for borders */
    const char *cls = html_attr(table, "class");

    if (cls && strstr(cls, "wikitable"))
        border = 1;

    for (int r = 0; r < nrows; r++) {

        int y0 = f->y, bottom = f->y, col = 0;
        int first_item = f->p->nitems;

        for (struct node *c = rows[r]->first; c; c = c->next) {

            if (c->type != N_ELEM || (strcmp(c->tag, "td") && strcmp(c->tag, "th")))
                continue;

            const char *span_text = html_attr(c, "colspan");
            int span = span_text && atoi(span_text) > 1 && atoi(span_text) < 20 ? atoi(span_text) : 1;

            if (col + span > cols)
                span = cols - col;

            if (span <= 0)
                break;

            struct flow cell = {f->p, f->left + col * width / cols + 4, f->left + (col + span) * width / cols - 4,
                                0, y0 + 3, f->p->nitems, 0, 0, 0, 0, 0};
            struct style q = *s;

            cell.x = cell.left;

            if (!strcmp(c->tag, "th")) {

                q.font = &font_bold;
                cell.center = 1;
            }

            flow_children(&cell, c, &q);
            end_line(&cell);

            if (cell.y > bottom)
                bottom = cell.y;

            col += span;
        }

        f->y = bottom + 3;

        if (border) {

            /* thin lines around and between the cells */
            struct item *it = add_item(f->p);

            if (it) {

                it->type = IT_RECT;
                it->x = f->left;
                it->y = y0;
                it->w = width;
                it->h = f->y - y0;
                it->color = 0xD0D7DE;
            }

            for (int k = 1; k < cols; k++) {

                it = add_item(f->p);

                if (it) {

                    it->type = IT_LINE;
                    it->x = f->left + k * width / cols;
                    it->y = y0;
                    it->w = 1;
                    it->h = f->y - y0;
                    it->color = 0xD0D7DE;
                }
            }
        }

        (void)first_item;
    }

    f->line_start = f->p->nitems;
    block_gap(f, 6);
}

static void flow_list_marker(struct flow *f, struct node *li, struct style *s) {

    struct node *list = li->parent;
    int ordered = list && !strcmp(list->tag, "ol");

    if (ordered) {

        int number = 1;
        const char *start = html_attr(list, "start");

        if (start)
            number = atoi(start);

        for (struct node *c = list->first; c && c != li; c = c->next)
            if (c->type == N_ELEM && !strcmp(c->tag, "li"))
                number++;

        char label[16];

        snprintf(label, sizeof(label), "%d.", number);

        struct item *it = add_item(f->p);

        if (it) {

            it->type = IT_TEXT;
            it->font = s->font;
            it->color = s->color;
            it->text = strdup(label);
            it->w = text_width(s->font, label);
            it->x = f->left - it->w - 6;
            it->h = s->font->height;
            it->ascent = s->font->ascent;
            it->link = -1;
        }
    }
    else {

        struct item *it = add_item(f->p);

        if (it) {

            it->type = IT_RECT;
            it->x = f->left - 12;
            it->w = 5;
            it->h = 5;
            it->ascent = s->font->ascent - 4;
            it->color = s->color;
            it->link = -2;     /* filled */
        }
    }

    /* the marker sits on the first line of the item */
    if (f->line_asc < s->font->ascent) {

        f->line_h += s->font->ascent - f->line_asc;
        f->line_asc = s->font->ascent;
    }
}

static void flow_node(struct flow *f, struct node *n, struct style *s) {

    if (n->type == N_TEXT) {

        if (n->text)
            flow_text(f, n->text, s);

        return;
    }

    if (is_hidden(n))
        return;

    const char *t = n->tag;
    struct style q = *s;
    int level = heading_level(t);
    const char *align = html_attr(n, "align");
    int center = !strcmp(t, "center") || (align && !strcasecmp(align, "center"));

    if (!strcmp(t, "br")) {

        if (f->line_h == 0) {

            f->line_h = s->font->height;
            f->line_asc = s->font->ascent;
        }

        end_line(f);
        return;
    }

    if (!strcmp(t, "img")) { flow_image(f, n, s); return; }
    if (!strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "textarea") || !strcmp(t, "select")) {

        flow_input(f, n, s);
        return;
    }

    if (!strcmp(t, "table")) { flow_table(f, n, s); return; }

    if (!strcmp(t, "hr")) {

        block_gap(f, 8);

        struct item *it = add_item(f->p);

        if (it) {

            it->type = IT_LINE;
            it->x = f->left;
            it->y = f->y;
            it->w = f->right - f->left;
            it->h = 1;
            it->color = 0xD0D7DE;
        }

        f->line_start = f->p->nitems;
        f->y += 1;
        block_gap(f, 8);
        return;
    }

    if (!strcmp(t, "a")) {

        const char *href = html_attr(n, "href");

        if (href && *href && *href != '#') {

            q.link = add_link(f->p, href);

            if (q.link >= 0) {

                q.color = C_LINK;
                q.underline = 1;
            }
        }
    }
    else if (!strcmp(t, "b") || !strcmp(t, "strong") || !strcmp(t, "th") || !strcmp(t, "dt") || !strcmp(t, "summary"))
        q.font = level ? q.font : &font_bold;
    else if (!strcmp(t, "i") || !strcmp(t, "em") || !strcmp(t, "cite") || !strcmp(t, "var") || !strcmp(t, "dfn"))
        q.font = q.font == &font_ui ? &font_italic : q.font;
    else if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") || !strcmp(t, "tt"))
        q.font = &font_mono;
    else if (!strcmp(t, "u") || !strcmp(t, "ins"))
        q.underline = 1;
    else if (!strcmp(t, "small") || !strcmp(t, "figcaption"))
        q.color = C_QUIET;
    else if (!strcmp(t, "q"))
        flow_text(f, "\"", s);

    /* blocks */
    int block = is_block(t) || level || !strcmp(t, "pre") || !strcmp(t, "blockquote") ||
                !strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "li") || !strcmp(t, "dd") || center;

    if (!block) {

        flow_children(f, n, &q);

        if (!strcmp(t, "q"))
            flow_text(f, "\"", s);

        return;
    }

    int gap = !strcmp(t, "p") ? 10 : level ? (level <= 2 ? 16 : 12) : (!strcmp(t, "ul") || !strcmp(t, "ol")) ? 8 :
              !strcmp(t, "pre") || !strcmp(t, "blockquote") ? 10 : 0;
    int old_left = f->left, old_right = f->right, old_center = f->center;

    block_gap(f, gap);

    if (level) {

        q.font = level == 1 ? &font_big : level == 2 ? &font_h2 : level == 3 ? &font_h3 : &font_bold;
        q.color = level <= 2 ? 0x0B1A33 : C_PAGE;
    }

    if (!strcmp(t, "pre")) {

        q.font = &font_mono;
        q.pre = 1;

        /* a light grey box behind code */
        struct item *bg = add_item(f->p);

        if (bg) {

            bg->type = IT_RECT;
            bg->x = f->left;
            bg->y = f->y;
            bg->w = f->right - f->left;
            bg->color = 0xF6F8FA;
            bg->link = -2;
        }

        f->line_start = f->p->nitems;
        f->left += 8;
        f->x = f->left;
        f->y += 6;

        int bg_index = (int)(bg - f->p->items);

        flow_children(f, n, &q);
        end_line(f);
        f->y += 6;
        f->p->items[bg_index].h = f->y - f->p->items[bg_index].y;
        f->left = old_left;
        f->x = f->left;
        block_gap(f, gap);
        return;
    }

    if (!strcmp(t, "blockquote")) { f->left += 24; f->right -= 16; q.color = 0x424A53; }
    if (!strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "dd")) f->left += 26;

    if (f->right - f->left < 80) {

        f->left = old_left;
        f->right = old_right;
    }

    if (center)
        f->center = 1;

    f->x = f->left;

    if (!strcmp(t, "li"))
        flow_list_marker(f, n, &q);

    if (center)
        q.center = 1;

    flow_children(f, n, &q);
    end_line(f);

    if (level == 1 || level == 2) {

        /* a line under big headings, like encyclopedias do */
        struct item *it = add_item(f->p);

        if (it) {

            it->type = IT_LINE;
            it->x = f->left;
            it->y = f->y;
            it->w = f->right - f->left;
            it->h = 1;
            it->color = 0xD8DEE4;
        }

        f->line_start = f->p->nitems;
        f->y += 2;
    }

    f->left = old_left;
    f->right = old_right;
    f->center = old_center;
    f->x = f->left;
    block_gap(f, gap ? gap : (!strcmp(t, "li") ? 2 : 0));
}

static void flow_children(struct flow *f, struct node *n, struct style *s) {

    for (struct node *c = n->first; c; c = c->next)
        flow_node(f, c, s);
}

/* ====================================================================
 * the page
 * ==================================================================== */

static void collect(struct page *p, struct node *n, int form) {

    for (; n; n = n->next) {

        if (n->type != N_ELEM)
            continue;

        int my_form = form;

        if (!strcmp(n->tag, "form")) {

            struct page_form *bigger = realloc(p->forms, sizeof(*bigger) * (size_t)(p->nforms + 1));

            if (bigger) {

                p->forms = bigger;

                const char *action = html_attr(n, "action"), *method = html_attr(n, "method");
                struct page_form *pf = &p->forms[p->nforms];

                if (url_join(p->base, action && *action ? action : p->base, pf->action, sizeof(pf->action)) != 0)
                    snprintf(pf->action, sizeof(pf->action), "%s", p->base);

                pf->post = method && !strcasecmp(method, "post");
                my_form = p->nforms++;
            }
        }
        else if (!strcmp(n->tag, "img")) {

            const char *src = html_attr(n, "src");
            char full[4200];

            /* lazy-loading pages keep the real picture in data-src */
            if ((!src || !strncmp(src, "data:", 5)) && html_attr(n, "data-src"))
                src = html_attr(n, "data-src");

            if (src && url_join(p->base, src, full, sizeof(full)) == 0 && strlen(full) < 2048) {

                struct page_image *bigger = realloc(p->images, sizeof(*bigger) * (size_t)(p->nimages + 1));

                if (bigger) {

                    p->images = bigger;

                    struct page_image *im = &p->images[p->nimages];
                    const char *w = html_attr(n, "width"), *h = html_attr(n, "height");

                    memset(im, 0, sizeof(*im));
                    snprintf(im->url, sizeof(im->url), "%s", full);
                    im->want_w = w ? atoi(w) : 0;
                    im->want_h = h ? atoi(h) : 0;

                    if (im->want_w > 4000 || im->want_w < 0) im->want_w = 0;
                    if (im->want_h > 4000 || im->want_h < 0) im->want_h = 0;

                    n->ref = ++p->nimages;
                }
            }
        }
        else if (!strcmp(n->tag, "input") || !strcmp(n->tag, "button") || !strcmp(n->tag, "textarea") ||
                 !strcmp(n->tag, "select")) {

            struct page_input *bigger = realloc(p->inputs, sizeof(*bigger) * (size_t)(p->ninputs + 1));

            if (bigger) {

                p->inputs = bigger;

                struct page_input *in = &p->inputs[p->ninputs];
                const char *type = html_attr(n, "type"), *name = html_attr(n, "name"), *value = html_attr(n, "value");

                memset(in, 0, sizeof(*in));
                in->form = my_form;
                snprintf(in->name, sizeof(in->name), "%s", name ? name : "");
                snprintf(in->value, sizeof(in->value), "%s", value ? value : "");

                if (!strcmp(n->tag, "textarea")) {

                    in->kind = IN_TEXTAREA;

                    if (n->first && n->first->type == N_TEXT && n->first->text)
                        snprintf(in->value, sizeof(in->value), "%s", n->first->text);
                }
                else if (!strcmp(n->tag, "select")) {

                    in->kind = IN_SELECT;

                    for (struct node *o = n->first; o; o = o->next) {

                        if (o->type != N_ELEM || strcmp(o->tag, "option"))
                            continue;

                        char **v = realloc(in->options, sizeof(char *) * (size_t)(in->noptions + 1));
                        char **tx = realloc(in->option_text, sizeof(char *) * (size_t)(in->noptions + 1));

                        if (!v || !tx)
                            break;

                        in->options = v;
                        in->option_text = tx;

                        const char *ov = html_attr(o, "value");
                        const char *ot = o->first && o->first->type == N_TEXT && o->first->text ? o->first->text : "";

                        in->options[in->noptions] = strdup(ov ? ov : ot);
                        in->option_text[in->noptions] = strdup(ot);

                        if (html_attr(o, "selected"))
                            in->chosen = in->noptions;

                        in->noptions++;
                    }
                }
                else if (!strcmp(n->tag, "button") || (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button") ||
                                                             !strcasecmp(type, "image") || !strcasecmp(type, "reset")))) {

                    in->kind = IN_SUBMIT;

                    char text[256] = "";

                    if (!strcmp(n->tag, "button")) {

                        /* the button's text: all text inside it */
                        for (struct node *c = n->first; c; c = c->next)
                            if (c->type == N_TEXT && c->text && strlen(text) + strlen(c->text) < sizeof(text) - 1)
                                strcat(text, c->text);
                    }

                    to_ascii(text[0] ? text : (value && *value ? value : "Submit"), in->label, sizeof(in->label));

                    /* trim */
                    char *l = in->label;

                    while (isspace((unsigned char)*l)) l++;
                    memmove(in->label, l, strlen(l) + 1);
                    for (size_t k = strlen(in->label); k && isspace((unsigned char)in->label[k - 1]); k--) in->label[k - 1] = 0;

                    if (!in->label[0])
                        snprintf(in->label, sizeof(in->label), "Submit");
                }
                else if (type && !strcasecmp(type, "password")) in->kind = IN_PASSWORD;
                else if (type && !strcasecmp(type, "checkbox")) { in->kind = IN_CHECKBOX; in->checked = html_attr(n, "checked") != NULL; }
                else if (type && !strcasecmp(type, "radio")) { in->kind = IN_RADIO; in->checked = html_attr(n, "checked") != NULL; }
                else if (type && !strcasecmp(type, "hidden")) in->kind = IN_HIDDEN;
                else in->kind = IN_TEXT;

                if (in->kind == IN_TEXT && !in->value[0] && html_attr(n, "placeholder"))
                    snprintf(in->label, sizeof(in->label), "%.120s", html_attr(n, "placeholder"));

                n->ref = ++p->ninputs;
            }
        }

        collect(p, n->first, my_form);
    }
}

void page_load(struct page *p, const char *base, const char *src, size_t len, int is_html) {

    memset(p, 0, sizeof(*p));
    p->focus = -1;
    snprintf(p->base, sizeof(p->base), "%s", base);

    if (is_html) {

        p->dom = html_parse(src, len, p->title, sizeof(p->title));

        /* <base href="..."> changes where relative links point */
        struct node *b = p->dom ? html_find(p->dom, "base") : NULL;
        const char *href = b ? html_attr(b, "href") : NULL;
        char full[4200];

        if (href && url_join(base, href, full, sizeof(full)) == 0)
            snprintf(p->base, sizeof(p->base), "%s", full);

        if (p->dom)
            collect(p, p->dom->first, -1);
    }
    else {

        /* plain text: shown as it is, in a fixed-width font */
        p->dom = calloc(1, sizeof(struct node));

        if (p->dom) {

            struct node *pre = calloc(1, sizeof(struct node)), *t = calloc(1, sizeof(struct node));

            if (pre && t) {

                pre->type = N_ELEM;
                snprintf(pre->tag, sizeof(pre->tag), "pre");
                pre->parent = p->dom;
                p->dom->first = p->dom->last = pre;
                t->type = N_TEXT;
                t->text = strndup(src, len);
                t->parent = pre;
                pre->first = pre->last = t;
            }
            else {

                free(pre);
                free(t);
            }
        }
    }

    char clean[256];

    to_ascii(p->title, clean, sizeof(clean));
    snprintf(p->title, sizeof(p->title), "%s", clean);
}

static void clear_layout(struct page *p) {

    for (int i = 0; i < p->nitems; i++)
        free(p->items[i].text);

    for (int i = 0; i < p->nlinks; i++)
        free(p->links[i]);

    p->nitems = 0;
    p->nlinks = 0;
}

void page_layout(struct page *p, int width) {

    clear_layout(p);

    if (width < 200)
        width = 200;

    /* long lines are hard to read: at most about 900 pixels of text */
    int text_w = width - 2 * MARGIN > 980 ? 980 : width - 2 * MARGIN;
    int left = (width - text_w) / 2;
    struct flow f = {p, left, left + text_w, left, MARGIN, 0, 0, 0, 0, 0, 0};
    struct style s = {&font_ui, C_PAGE, 0, -1, 0, 0, -1};

    if (p->dom)
        flow_children(&f, p->dom, &s);

    end_line(&f);
    p->height = f.y + MARGIN;
    p->width = width;
}

void page_free(struct page *p) {

    clear_layout(p);
    free(p->items);
    free(p->links);

    for (int i = 0; i < p->nimages; i++)
        free(p->images[i].img.px);

    for (int i = 0; i < p->ninputs; i++) {

        for (int k = 0; k < p->inputs[i].noptions; k++) {

            free(p->inputs[i].options[k]);
            free(p->inputs[i].option_text[k]);
        }

        free(p->inputs[i].options);
        free(p->inputs[i].option_text);
    }

    free(p->images);
    free(p->inputs);
    free(p->forms);
    html_free(p->dom);
    memset(p, 0, sizeof(*p));
    p->focus = -1;
}

/* ====================================================================
 * drawing
 * ==================================================================== */

static void draw_input(struct page *p, struct item *it, int x, int y) {

    struct page_input *in = &p->inputs[it->input];
    int focus = it->input == p->focus;
    char shown[600];

    switch (in->kind) {

        case IN_SUBMIT:
            fill_round(x, y, it->w, it->h, 5, 0xF3F4F6);
            frame(x, y, it->w, it->h, 0xD0D7DE);
            text(&font_ui, x + 12, y + 5, in->label, C_PAGE);
            break;

        case IN_CHECKBOX:
        case IN_RADIO:
            fill_round(x, y, 16, 16, in->kind == IN_RADIO ? 8 : 3, 0x8C959F);
            fill_round(x + 1, y + 1, 14, 14, in->kind == IN_RADIO ? 7 : 2, C_WHITE);

            if (in->checked)
                fill_round(x + 4, y + 4, 8, 8, in->kind == IN_RADIO ? 4 : 1, C_LINK);
            break;

        case IN_SELECT: {

            fill_round(x, y, it->w, it->h, 5, 0xF6F8FA);
            frame(x, y, it->w, it->h, 0xD0D7DE);
            to_ascii(in->noptions ? in->option_text[in->chosen] : "", shown, sizeof(shown));
            text(&font_ui, x + 8, y + 5, shown, C_PAGE);
            text(&font_bold, x + it->w - 16, y + 5, "v", C_QUIET);
            break;
        }

        default: {

            fill(x, y, it->w, it->h, C_WHITE);
            frame(x, y, it->w, it->h, focus ? C_LINK : 0x8C959F);

            if (in->kind == IN_PASSWORD) {

                size_t n = strlen(in->value);

                memset(shown, '*', n < sizeof(shown) - 1 ? n : sizeof(shown) - 1);
                shown[n < sizeof(shown) - 1 ? n : sizeof(shown) - 1] = 0;
            }
            else
                to_ascii(in->value, shown, sizeof(shown));

            const char *start = shown;

            while (*start && text_width(&font_ui, start) > it->w - 14)
                start++;

            if (!shown[0] && in->label[0] && !focus) {

                char ph[200];

                to_ascii(in->label, ph, sizeof(ph));
                text(&font_ui, x + 6, y + 5, ph, 0x8C959F);
            }
            else
                text(&font_ui, x + 6, y + 5, start, C_PAGE);

            if (focus)
                fill(x + 7 + text_width(&font_ui, start), y + 5, 1, 16, C_PAGE);
        }
    }
}

void page_draw(struct page *p, int x, int y, int w, int h, int scroll) {

    fill(x, y, w, h, C_WHITE);

    for (int i = 0; i < p->nitems; i++) {

        struct item *it = &p->items[i];
        int ix = x + it->x, iy = y + it->y - scroll;

        if (iy > y + h || iy + it->h < y)
            continue;

        switch (it->type) {

            case IT_TEXT:
                text(it->font, ix, iy, it->text, it->color);

                if (it->underline)
                    fill(ix, iy + it->ascent + 2, it->w, 1, it->color);
                break;

            case IT_RECT:
                if (it->link == -2)
                    fill(ix, iy, it->w, it->h, it->color);
                else
                    frame(ix, iy, it->w, it->h, it->color);
                break;

            case IT_LINE:
                fill(ix, iy, it->w, it->h, it->color);
                break;

            case IT_IMAGE: {

                struct page_image *im = &p->images[it->image];

                if (im->state == 2 && im->img.px && im->img.w == it->w && im->img.h == it->h)
                    draw_image(&im->img, ix, iy);
                else {

                    fill(ix, iy, it->w, it->h, 0xEEF1F4);
                    frame(ix, iy, it->w, it->h, 0xD0D7DE);
                }

                if (it->link >= 0)
                    frame(ix, iy, it->w, it->h, C_LINK);
                break;
            }

            case IT_INPUT:
                draw_input(p, it, ix, iy);
                break;
        }
    }
}

int page_link_at(struct page *p, int px, int py) {

    for (int i = p->nitems - 1; i >= 0; i--) {

        struct item *it = &p->items[i];

        if (it->link >= 0 && px >= it->x && px < it->x + it->w && py >= it->y - 2 && py < it->y + it->h + 2)
            return it->link;
    }

    return -1;
}

int page_input_at(struct page *p, int px, int py) {

    for (int i = 0; i < p->nitems; i++) {

        struct item *it = &p->items[i];

        if (it->type == IT_INPUT && px >= it->x && px < it->x + it->w && py >= it->y && py < it->y + it->h)
            return it->input;
    }

    return -1;
}

int page_find(struct page *p, const char *what, int after_y) {

    for (int i = 0; i < p->nitems; i++) {

        struct item *it = &p->items[i];

        if (it->type == IT_TEXT && it->y > after_y && strcasestr(it->text, what))
            return it->y;
    }

    return -1;
}
