#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "render.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <emmintrin.h>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void png_write_fn(void *ctx, void *data, int sz) {
    fwrite(data, 1, (size_t)sz, (FILE*)ctx);
}

int image_save_png(image *img, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    stbi_write_png_to_func(png_write_fn, f, img->width, img->height, 4, img->data, img->width * 4);
    fclose(f);
    return 0;
}

/* Image pool for 72x72 images to reduce malloc/free */
#define IMG_POOL_SIZE 32
static image g_img_pool[IMG_POOL_SIZE];
static uint8_t g_img_pool_data[IMG_POOL_SIZE][72*72*4];
static int g_img_pool_idx = 0;

static void pool_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    for (int i = 0; i < IMG_POOL_SIZE; i++) {
        g_img_pool[i].width = 72;
        g_img_pool[i].height = 72;
        g_img_pool[i].channels = 4;
        g_img_pool[i].data = g_img_pool_data[i];
    }
}

image *image_pool_get(void) {
    pool_init();
    image *img = &g_img_pool[g_img_pool_idx];
    g_img_pool_idx = (g_img_pool_idx + 1) % IMG_POOL_SIZE;
    return img;
}

image *image_create(int w, int h) {
    image *img = calloc(1, sizeof(image));
    img->width = w;
    img->height = h;
    img->channels = 4;
    img->data = calloc((size_t)(w * h * 4), 1);
    return img;
}

void image_free(image *img) {
    if (!img) return;
    if (img->data >= (uint8_t*)g_img_pool_data[0] &&
        img->data <= (uint8_t*)g_img_pool_data[IMG_POOL_SIZE-1] + 72*72*4)
        return;
    free(img->data);
    free(img);
}

int image_load(const char *path, image *img) {
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    if (!data) {
        errorLog("stbi_load failed: %s", stbi_failure_reason());
        return -1;
    }
    img->width = w;
    img->height = h;
    img->channels = 4;
    free(img->data);
    img->data = data;
    return 0;
}

void render_fill(image *img, uint8_t r, uint8_t g, uint8_t b) {
    size_t total = (size_t)img->width * img->height * 4;
    __m128i color = _mm_setr_epi8(
        r, g, b, 255, r, g, b, 255, r, g, b, 255, r, g, b, 255
    );
    size_t i;
    for (i = 0; i + 16 <= total; i += 16)
        _mm_storeu_si128((__m128i*)(img->data + i), color);
    for (; i < total; i += 4) {
        img->data[i] = r;
        img->data[i+1] = g;
        img->data[i+2] = b;
        img->data[i+3] = 255;
    }
}

void render_rect(image *img, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b) {
    for (int py = y; py < y + h && py < img->height; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < img->width; px++) {
            if (px < 0) continue;
            size_t off = (size_t)(py * img->width + px) * 4;
            img->data[off] = r;
            img->data[off+1] = g;
            img->data[off+2] = b;
            img->data[off+3] = 255;
        }
    }
}

void render_circle_progress(image *img, int cx, int cy, int radius,
                            float progress, uint8_t r, uint8_t g, uint8_t b) {
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;

    float end_angle = -M_PI_2 + 2.0f * M_PI * progress;

    for (int py = cy - radius; py <= cy + radius; py++) {
        for (int px = cx - radius; px <= cx + radius; px++) {
            int dx = px - cx;
            int dy = py - cy;
            float dist = sqrtf((float)(dx*dx + dy*dy));
            if (dist > (float)radius || dist < (float)(radius - 3)) continue;
            float angle = atan2f((float)dy, (float)dx);
            if (angle <= end_angle && angle > -M_PI_2 * 1.01f) {
                size_t off = (size_t)(py * img->width + px) * 4;
                img->data[off] = r;
                img->data[off+1] = g;
                img->data[off+2] = b;
                img->data[off+3] = 255;
            }
        }
    }
}

void render_text(image *img, const char *text, int x, int y,
                 uint8_t r, uint8_t g, uint8_t b) {
    (void)img; (void)text; (void)x; (void)y; (void)r; (void)g; (void)b;
}

static font_info g_font;
static unsigned char *g_font_data = NULL;
static int g_font_loaded = 0;

/* Glyph cache: key = (codepoint << 8) | (int)(size*2), stores bitmap + metrics */
#define GLYPH_CACHE_SIZE 64
typedef struct { int key; unsigned char *bitmap; int w, h, xoff, yoff, adv; } glyph_cache_entry;
static glyph_cache_entry g_glyph_cache[GLYPH_CACHE_SIZE];
static int g_glyph_cache_idx = 0;

static glyph_cache_entry *glyph_cache_get(int codepoint, float scale, int *adv) {
    int key = (codepoint << 8) | ((int)(scale * 200) & 0xFF);
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (g_glyph_cache[i].key == key) {
            *adv = g_glyph_cache[i].adv;
            return &g_glyph_cache[i];
        }
    }
    /* Evict oldest */
    int idx = g_glyph_cache_idx++ % GLYPH_CACHE_SIZE;
    glyph_cache_entry *e = &g_glyph_cache[idx];
    if (e->bitmap) stbtt_FreeBitmap(e->bitmap, NULL);
    e->key = key;
    int lsb;
    stbtt_GetCodepointHMetrics(&g_font, codepoint, &e->adv, &lsb);
    e->bitmap = stbtt_GetCodepointBitmap(&g_font, scale, scale, codepoint, &e->w, &e->h, &e->xoff, &e->yoff);
    *adv = e->adv;
    return e;
}

int font_init(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { errorLog("font_init: cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    free(g_font_data);
    g_font_data = malloc((size_t)sz);
    fread(g_font_data, 1, (size_t)sz, f);
    fclose(f);

    if (sz > 4 && g_font_data[0] == 't' && g_font_data[1] == 't' && g_font_data[2] == 'c' && g_font_data[3] == 0x01) {
        unsigned major = (unsigned)(g_font_data[4]) | ((unsigned)(g_font_data[5]) << 8);
        unsigned num_fonts = (unsigned)(g_font_data[8]) | ((unsigned)(g_font_data[9]) << 8) |
                            ((unsigned)(g_font_data[10]) << 16) | ((unsigned)(g_font_data[11]) << 24);
        unsigned offset_base = 12;
        long long offset;
        if (major >= 2) {
            offset_base = 16;
        }
        if (num_fonts > 0) {
            if (major >= 2) {
                offset = (long long)(g_font_data[offset_base]) |
                         ((long long)(g_font_data[offset_base+1]) << 8) |
                         ((long long)(g_font_data[offset_base+2]) << 16) |
                         ((long long)(g_font_data[offset_base+3]) << 24) |
                         ((long long)(g_font_data[offset_base+4]) << 32) |
                         ((long long)(g_font_data[offset_base+5]) << 40) |
                         ((long long)(g_font_data[offset_base+6]) << 48) |
                         ((long long)(g_font_data[offset_base+7]) << 56);
            } else {
                offset = (unsigned)(g_font_data[offset_base]) |
                         ((unsigned)(g_font_data[offset_base+1]) << 8) |
                         ((unsigned)(g_font_data[offset_base+2]) << 16) |
                         ((unsigned)(g_font_data[offset_base+3]) << 24);
            }
            if (stbtt_InitFont(&g_font, g_font_data + offset, 0)) {
                g_font_loaded = 1;
                infoLog("Font loaded from TTC: %s (font #0)", path);
                return 0;
            }
        }
    }

    if (stbtt_InitFont(&g_font, g_font_data, 0)) {
        g_font_loaded = 1;
        infoLog("Font loaded: %s", path);
        return 0;
    }

    errorLog("stbtt_InitFont failed: %s", path);
    return -1;
}

int font_measure(const char *text, float size) {
    if (!g_font_loaded) {
        int len = 0;
        for (const char *p = text; *p; p++) len++;
        return len * (int)(size * 0.6f + 2);
    }

    /* Simple cache: same text pointer + size */
    static const char *prev_text = NULL;
    static float prev_size = 0;
    static int prev_result = 0;
    if (text == prev_text && size == prev_size) return prev_result;
    prev_text = text;
    prev_size = size;

    float scale = stbtt_ScaleForPixelHeight(&g_font, size);
    int total = 0;
    for (const char *p = text; *p; ) {
        unsigned c = (unsigned char)*p;
        int adv;
        if (c < 0x80) {
            p++;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            c = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
            p += 2;
        } else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
            p += 3;
        } else {
            p++;
        }
        stbtt_GetCodepointHMetrics(&g_font, (int)c, &adv, NULL);
        total += (int)(adv * scale);
    }
    prev_result = total;
    return total;
}

void font_draw(image *img, const char *text, int x, int y,
               uint8_t r, uint8_t g, uint8_t b, float size) {
    if (!g_font_loaded || !text || !*text) return;

    float scale = stbtt_ScaleForPixelHeight(&g_font, size);
    int ascent;
    stbtt_GetFontVMetrics(&g_font, &ascent, NULL, NULL);
    int baseline = y + (int)(ascent * scale);

    int bx = x;
    for (const char *p = text; *p; ) {
        unsigned c = (unsigned char)*p;
        if (c < 0x80) {
            p++;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            c = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
            p += 2;
        } else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
            p += 3;
        } else {
            p++;
        }

        int adv;
        glyph_cache_entry *gce = glyph_cache_get((int)c, scale, &adv);
        if (!gce || !gce->bitmap) { bx += (int)(adv * scale); continue; }

        int ox = bx + gce->xoff;
        int oy = baseline + gce->yoff;
        for (int row = 0; row < gce->h; row++) {
            for (int col = 0; col < gce->w; col++) {
                unsigned char a = gce->bitmap[row * gce->w + col];
                if (a == 0) continue;
                int px = ox + col;
                int py = oy + row;
                if (px < 0 || px >= img->width || py < 0 || py >= img->height) continue;
                size_t off = (size_t)(py * img->width + px) * 4;
                if (a == 255) {
                    img->data[off] = r;
                    img->data[off+1] = g;
                    img->data[off+2] = b;
                    img->data[off+3] = 255;
                } else {
                    uint8_t ia = 255 - a;
                    img->data[off] = (uint8_t)(((unsigned)img->data[off] * ia + (unsigned)r * a) / 255);
                    img->data[off+1] = (uint8_t)(((unsigned)img->data[off+1] * ia + (unsigned)g * a) / 255);
                    img->data[off+2] = (uint8_t)(((unsigned)img->data[off+2] * ia + (unsigned)b * a) / 255);
                    img->data[off+3] = 255;
                }
            }
        }
        bx += (int)(adv * scale);
    }
}
