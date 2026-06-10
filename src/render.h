#ifndef RENDER_H
#define RENDER_H

#include "common.h"
#include <stddef.h>

typedef struct {
    int width, height, channels;
    uint8_t *data;
} image;

typedef struct stbtt_fontinfo font_info;

image *image_create(int w, int h);
void   image_free(image *img);
image *image_pool_get(void);
int    image_load(const char *path, image *img);

void render_fill(image *img, uint8_t r, uint8_t g, uint8_t b);
void render_rect(image *img, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b);
void render_circle_progress(image *img, int cx, int cy, int radius,
                            float progress, uint8_t r, uint8_t g, uint8_t b);
void render_text(image *img, const char *text, int x, int y,
                 uint8_t r, uint8_t g, uint8_t b);

int  font_init(const char *path);
void font_draw(image *img, const char *text, int x, int y, uint8_t r, uint8_t g, uint8_t b, float size);
int  font_measure(const char *text, float size);
int  image_save_png(image *img, const char *path);

#endif
