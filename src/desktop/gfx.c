/*
 * gfx.c - drawing on the screen.
 *
 * Everything is first drawn into "back", a picture in normal memory with
 * one 32-bit number per pixel (0xRRGGBB). When a frame is finished,
 * gfx_flush_rect() copies it to the real screen, so the user never sees a
 * half-drawn picture (no flicker).
 *
 * To save work, only the part of the screen that changed is redrawn:
 * gfx_area() limits all drawing to that box ("damaged area").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "desktop.h"
#include "font_data.h"

struct font font_ui   = {ui_widths,      ui_offsets,      ui_alpha,      UI_HEIGHT,      UI_ASCENT};
struct font font_bold = {ui_bold_widths, ui_bold_offsets, ui_bold_alpha, UI_BOLD_HEIGHT, UI_BOLD_ASCENT};
struct font font_mono = {mono_widths,    mono_offsets,    mono_alpha,    MONO_HEIGHT,    MONO_ASCENT};
struct font font_big  = {big_widths,     big_offsets,     big_alpha,     BIG_HEIGHT,     BIG_ASCENT};

int screen_w, screen_h;

static int fb_fd = -1;
static uint8_t *fb_mem;              /* the real screen memory */
static size_t fb_size;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;

static uint32_t *back;               /* the picture we draw into */
static uint32_t *saved;              /* a copy for save/restore_background */

/* the drawing area allowed right now */
static int clip_x0, clip_y0, clip_x1, clip_y1;

/* the part of the screen being redrawn; clips never go outside it */
static int area_x0, area_y0, area_x1, area_y1;

/* ---------- opening and showing ---------- */

int gfx_open(void) {

    fb_fd = open("/dev/fb0", O_RDWR);

    if (fb_fd < 0)
        return -errno;

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0) {

        close(fb_fd);
        return -EINVAL;
    }

    if (var.bits_per_pixel != 32 &&
        var.bits_per_pixel != 24 &&
        var.bits_per_pixel != 16) {

        close(fb_fd);
        return -ENOTSUP;
    }

    screen_w = var.xres;
    screen_h = var.yres;

    fb_size = (size_t)fix.line_length * var.yres_virtual;

    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    if (fb_mem == MAP_FAILED) {

        close(fb_fd);
        return -ENOMEM;
    }

    back  = calloc((size_t)screen_w * screen_h, sizeof(uint32_t));
    saved = calloc((size_t)screen_w * screen_h, sizeof(uint32_t));

    if (!back || !saved)
        return -ENOMEM;

    gfx_area(0, 0, screen_w, screen_h);

    return 0;
}

void gfx_close(void) {

    if (fb_mem && fb_mem != MAP_FAILED) {

        /* leave a black screen behind for the text console */
        memset(fb_mem, 0, fb_size);
        munmap(fb_mem, fb_size);
    }

    if (fb_fd >= 0)
        close(fb_fd);

    free(back);
    free(saved);
}

/* Copy one box of the finished picture to the screen, in its format. */
void gfx_flush_rect(int x, int y, int w, int h) {

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;

    if (w <= 0 || h <= 0)
        return;

    int bpp = var.bits_per_pixel;

    /* the most common case: 32 bits, red-green-blue like ours */
    int same = bpp == 32 &&
               var.red.offset == 16 &&
               var.green.offset == 8 &&
               var.blue.offset == 0;

    for (int row = y; row < y + h; row++) {

        uint32_t *src = back + (size_t)row * screen_w + x;
        uint8_t *dst = fb_mem +
                       (size_t)(row + var.yoffset) * fix.line_length +
                       (size_t)(x + var.xoffset) * (bpp / 8);

        if (same) {

            memcpy(dst, src, (size_t)w * 4);
            continue;
        }

        for (int i = 0; i < w; i++) {

            uint32_t c = src[i];
            uint32_t r = (c >> 16) & 255;
            uint32_t g = (c >> 8) & 255;
            uint32_t b = c & 255;

            /* shrink each colour to the number of bits the screen has */
            uint32_t p = ((r >> (8 - var.red.length)) << var.red.offset) |
                         ((g >> (8 - var.green.length)) << var.green.offset) |
                         ((b >> (8 - var.blue.length)) << var.blue.offset);

            if (bpp == 32)
                ((uint32_t *)dst)[i] = p;
            else if (bpp == 24) {

                dst[i * 3]     = p & 255;
                dst[i * 3 + 1] = (p >> 8) & 255;
                dst[i * 3 + 2] = (p >> 16) & 255;
            }
            else
                ((uint16_t *)dst)[i] = (uint16_t)p;
        }
    }
}

void gfx_flush(void) {

    gfx_flush_rect(0, 0, screen_w, screen_h);
}

void save_background(void) {

    memcpy(saved, back, (size_t)screen_w * screen_h * 4);
}

/* Paint the saved picture back, but only inside the current area. */
void restore_background(void) {

    int w = area_x1 - area_x0;

    for (int y = area_y0; y < area_y1 && w > 0; y++) {

        size_t at = (size_t)y * screen_w + area_x0;

        memcpy(back + at, saved + at, (size_t)w * 4);
    }
}

/* ---------- clipping ---------- */

void gfx_area(int x, int y, int w, int h) {

    area_x0 = x < 0 ? 0 : x;
    area_y0 = y < 0 ? 0 : y;
    area_x1 = x + w > screen_w ? screen_w : x + w;
    area_y1 = y + h > screen_h ? screen_h : y + h;

    gfx_noclip();
}

void gfx_noclip(void) {

    clip_x0 = area_x0;
    clip_y0 = area_y0;
    clip_x1 = area_x1;
    clip_y1 = area_y1;
}

int gfx_visible(int x, int y, int w, int h) {

    return x < clip_x1 && y < clip_y1 && x + w > clip_x0 && y + h > clip_y0;
}

void gfx_clip(int x, int y, int w, int h) {

    gfx_noclip();

    if (x > clip_x0) clip_x0 = x;
    if (y > clip_y0) clip_y0 = y;
    if (x + w < clip_x1) clip_x1 = x + w;
    if (y + h < clip_y1) clip_y1 = y + h;
}

/* ---------- pixels and shapes ---------- */

static inline uint32_t mix(uint32_t dst, uint32_t src, int a) {

    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((src & 255) * a + (dst & 255) * (255 - a)) / 255;

    return (r << 16) | (g << 8) | b;
}

void put_pixel(int x, int y, uint32_t c, int alpha) {

    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1)
        return;

    uint32_t *p = &back[(size_t)y * screen_w + x];

    *p = alpha >= 255 ? c : mix(*p, c, alpha);
}

/* Cut a box down to the clip area. Returns 0 if nothing is left. */
static int cut(int *x, int *y, int *w, int *h) {

    int x1 = *x + *w;
    int y1 = *y + *h;

    if (*x < clip_x0) *x = clip_x0;
    if (*y < clip_y0) *y = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;

    *w = x1 - *x;
    *h = y1 - *y;

    return *w > 0 && *h > 0;
}

void fill(int x, int y, int w, int h, uint32_t c) {

    if (!cut(&x, &y, &w, &h))
        return;

    for (int j = 0; j < h; j++) {

        uint32_t *p = &back[(size_t)(y + j) * screen_w + x];

        for (int i = 0; i < w; i++)
            p[i] = c;
    }
}

void fill_alpha(int x, int y, int w, int h, uint32_t c, int alpha) {

    if (!cut(&x, &y, &w, &h))
        return;

    for (int j = 0; j < h; j++) {

        uint32_t *p = &back[(size_t)(y + j) * screen_w + x];

        for (int i = 0; i < w; i++)
            p[i] = mix(p[i], c, alpha);
    }
}

void frame(int x, int y, int w, int h, uint32_t c) {

    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

void gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {

    for (int j = 0; j < h; j++)
        fill(x, y + j, w, 1, mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

/* A box with rounded corners. The corner edges are softened. */
void fill_round(int x, int y, int w, int h, int r, uint32_t c) {

    if (!gfx_visible(x, y, w, h))
        return;

    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;

    fill(x, y + r, w, h - 2 * r, c);

    for (int j = 0; j < r; j++) {

        /* how far the curved corner is from the box edge on this row */
        double dy = r - j - 0.5;
        double dx = r - sqrt((double)r * r - dy * dy);

        int inset = (int)dx;
        int soft = (int)((1.0 - (dx - inset)) * 255);

        fill(x + inset + 1, y + j, w - 2 * inset - 2, 1, c);
        fill(x + inset + 1, y + h - 1 - j, w - 2 * inset - 2, 1, c);

        put_pixel(x + inset, y + j, c, soft);
        put_pixel(x + w - 1 - inset, y + j, c, soft);
        put_pixel(x + inset, y + h - 1 - j, c, soft);
        put_pixel(x + w - 1 - inset, y + h - 1 - j, c, soft);
    }
}

void fill_circle(int cx, int cy, int r, uint32_t c) {

    if (!gfx_visible(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3))
        return;

    for (int y = -r; y <= r; y++) {

        for (int x = -r; x <= r; x++) {

            double d = sqrt((double)x * x + (double)y * y);

            if (d <= r - 0.5)
                put_pixel(cx + x, cy + y, c, 255);
            else if (d < r + 0.5)
                put_pixel(cx + x, cy + y, c, (int)((r + 0.5 - d) * 255));
        }
    }
}

/* Straight line (Bresenham's method: only whole-pixel steps). */
void line(int x0, int y0, int x1, int y1, uint32_t c) {

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {

        put_pixel(x0, y0, c, 255);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;

        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---------- text ---------- */

void text_char(struct font *f, int x, int y, char ch, uint32_t c) {

    unsigned char u = (unsigned char)ch;

    if (u < 32 || u > 126)
        u = '?';

    int i = u - 32;
    int w = f->widths[i];
    const unsigned char *a = f->alpha + f->offsets[i];

    /* nothing to do for a letter outside the area being drawn */
    if (!gfx_visible(x, y, w, f->height))
        return;

    for (int j = 0; j < f->height; j++) {

        for (int k = 0; k < w; k++) {

            int alpha = a[j * w + k];

            if (alpha)
                put_pixel(x + k, y + j, c, alpha);
        }
    }
}

int text_width(struct font *f, const char *s) {

    int w = 0;

    for (; *s; s++) {

        unsigned char u = (unsigned char)*s;

        w += f->widths[(u < 32 || u > 126 ? '?' : u) - 32];
    }

    return w;
}

int text_alpha(struct font *f, int x, int y, const char *s, uint32_t c, int alpha) {

    int start = x;

    for (; *s; s++) {

        unsigned char u = (unsigned char)*s;
        int i = (u < 32 || u > 126 ? '?' : u) - 32;
        int w = f->widths[i];
        const unsigned char *a = f->alpha + f->offsets[i];

        /* skip letters that are completely outside the clip area */
        if (gfx_visible(x, y, w, f->height)) {

            for (int j = 0; j < f->height; j++) {

                for (int k = 0; k < w; k++) {

                    int v = a[j * w + k];

                    if (v)
                        put_pixel(x + k, y + j, c, v * alpha / 255);
                }
            }
        }

        x += w;
    }

    return x - start;
}

int text(struct font *f, int x, int y, const char *s, uint32_t c) {

    return text_alpha(f, x, y, s, c, 255);
}

/* ---------- pictures ---------- */

/*
 * Loads a .img file (made by tools/make-logo.py): width, height, then
 * red, green, blue, alpha for every pixel. Returns NULL if it is missing.
 */
struct image *image_load(const char *path) {

    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;

    uint32_t size[2];
    struct image *img = NULL;

    if (fread(size, 4, 2, f) == 2 &&
        size[0] > 0 && size[0] <= 4096 && size[1] > 0 && size[1] <= 4096) {

        img = malloc(sizeof(*img));

        if (img) {

            img->w = (int)size[0];
            img->h = (int)size[1];
            img->px = malloc((size_t)img->w * img->h * 4);

            if (!img->px ||
                fread(img->px, 4, (size_t)img->w * img->h, f) != (size_t)img->w * img->h) {

                free(img->px);
                free(img);
                img = NULL;
            }
        }
    }

    fclose(f);

    return img;
}

void draw_image(struct image *img, int x, int y) {

    if (!img || !gfx_visible(x, y, img->w, img->h))
        return;

    for (int j = 0; j < img->h; j++) {

        const uint8_t *p = img->px + (size_t)j * img->w * 4;

        for (int i = 0; i < img->w; i++, p += 4) {

            if (p[3])
                put_pixel(x + i, y + j, ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2], p[3]);
        }
    }
}

/* ---------- logo and mouse pointer ---------- */

#define LOGO_DIR "/usr/share/bharat/"

/* The round penguin badge from the Bharat Linux logo. */
void draw_logo(int x, int y, int size) {

    static struct image *small, *medium;
    static int loaded;

    if (!loaded) {

        small = image_load(LOGO_DIR "mark26.img");
        medium = image_load(LOGO_DIR "mark42.img");
        loaded = 1;
    }

    struct image *img = size <= 30 ? small : medium;

    if (img && img->w <= size + 4) {

        draw_image(img, x + (size - img->w) / 2, y + (size - img->h) / 2);
        return;
    }

    /* no picture installed: a saffron tile with a white "B" */
    int r = size / 4;

    fill_round(x, y, size, size, r, C_SAFFRON_D);
    fill_round(x + 1, y + 1, size - 2, size - 2 - size / 8, r, C_SAFFRON);

    struct font *f = size >= 40 ? &font_big : &font_bold;
    int tw = text_width(f, "B");

    text(f, x + (size - tw) / 2, y + (size - f->height) / 2, "B", C_WHITE);
}

/* An arrow pointer: X = outline, . = fill */
static const char *cursor_shape[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X......XXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     X..X",
    "      X..X",
    "       XX",
};

void draw_cursor(int x, int y) {

    gfx_noclip();

    if (!gfx_visible(x, y, CURSOR_W, CURSOR_H))
        return;

    for (int j = 0; j < (int)(sizeof(cursor_shape) / sizeof(*cursor_shape)); j++) {

        for (int i = 0; cursor_shape[j][i]; i++) {

            if (cursor_shape[j][i] == 'X')
                put_pixel(x + i, y + j, 0x000000, 255);
            else if (cursor_shape[j][i] == '.')
                put_pixel(x + i, y + j, C_WHITE, 255);
        }
    }
}
