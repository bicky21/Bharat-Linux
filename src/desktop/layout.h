#ifndef LAYOUT_H
#define LAYOUT_H

#include "desktop.h"
#include "html.h"

enum { IT_TEXT, IT_RECT, IT_IMAGE, IT_INPUT, IT_LINE };

struct item {
    short type;
    int x, y, w, h;
    int ascent;
    struct font *font;
    uint32_t color;
    char *text;            /* IT_TEXT */
    int underline;
    int link;              /* index into page links, or -1 */
    int image;             /* IT_IMAGE: index into page images */
    int input;             /* IT_INPUT: index into page inputs */
};

struct page_image {
    char url[2048];
    int want_w, want_h;    /* from width= / height= (0: unknown) */
    int state;             /* 0 waiting, 1 asked, 2 loaded, 3 failed */
    struct image img;
};

enum { IN_TEXT, IN_PASSWORD, IN_SUBMIT, IN_CHECKBOX, IN_RADIO, IN_HIDDEN, IN_TEXTAREA, IN_SELECT };

struct page_input {
    int form;
    int kind;
    char name[128];
    char value[512];
    char label[128];       /* text shown on buttons */
    int checked;
    char **options;        /* <select>: values, then shown texts */
    char **option_text;
    int noptions, chosen;
};

struct page_form {
    char action[4200];
    int post;
};

struct page {
    struct node *dom;
    char base[4200];       /* the page's address, for relative links */
    char title[256];
    struct item *items;
    int nitems, cap;
    char **links;
    int nlinks, link_cap;
    struct page_image *images;
    int nimages;
    struct page_input *inputs;
    int ninputs;
    struct page_form *forms;
    int nforms;
    int height;            /* of the whole laid-out page */
    int width;             /* it was laid out for */
    int focus;             /* input being typed into, or -1 */
};

/* builds the page from HTML (or plain text when !is_html) */
void page_load(struct page *p, const char *base, const char *src, size_t len, int is_html);
void page_layout(struct page *p, int width);
void page_free(struct page *p);
void page_draw(struct page *p, int x, int y, int w, int h, int scroll);
int  page_link_at(struct page *p, int px, int py);     /* page coordinates */
int  page_input_at(struct page *p, int px, int py);
int  page_find(struct page *p, const char *what, int after_y);   /* y of the next match, or -1 */

/* turns UTF-8 into what our fonts can show */
void to_ascii(const char *in, char *out, size_t size);

#endif
