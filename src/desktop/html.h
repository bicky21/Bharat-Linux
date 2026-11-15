#ifndef HTML_H
#define HTML_H

#include <stddef.h>

enum { N_ELEM, N_TEXT };

struct attr {
    char *name;
    char *value;
};

struct node {
    int type;
    char tag[16];               /* lower case, for elements */
    char *text;                 /* for text: UTF-8, entities decoded */
    struct attr *attrs;
    int nattrs;
    int ref;                    /* the page's index for it + 1 (img, input) */
    struct node *parent, *first, *last, *next;
};

struct node *html_parse(const char *src, size_t len, char *title, size_t title_size);
void html_free(struct node *n);
const char *html_attr(const struct node *n, const char *name);
struct node *html_find(struct node *n, const char *tag);

#endif
