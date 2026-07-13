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

/* ---------- ユーティリティ ---------- */

/* 255 での除算を乗算+シフトに置き換え (x < 2^23 で厳密) */
static inline uint8_t div255(unsigned x) {
    return (uint8_t)((x * 32897U) >> 23);
}

/* 32bit RGBA パック */
static inline uint32_t rgba32(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* UTF-8 1文字デコード */
static inline const char *decode_utf8(const char *p, unsigned *out_cp) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) {
        *out_cp = c;
        return p + 1;
    }
    if ((c & 0xE0) == 0xC0 && p[1]) {
        *out_cp = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
        return p + 2;
    }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *out_cp = ((unsigned)(p[0] & 0x0F) << 12)
                | ((unsigned)(p[1] & 0x3F) << 6)
                |  (unsigned)(p[2] & 0x3F);
        return p + 3;
    }
    *out_cp = c;
    return p + 1;
}

/* ---------- PNG 出力 ---------- */

static void png_write_fn(void *ctx, void *data, int sz) {
    fwrite(data, 1, (size_t)sz, (FILE*)ctx);
}

int image_save_png(image *img, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    stbi_write_png_to_func(png_write_fn, f,
                           img->width, img->height, 4,
                           img->data, img->width * 4);
    fclose(f);
    return 0;
}

/* ---------- Image Pool ---------- */

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

/* img ポインタがプール配列内かで判定 (data ポインタ比較より堅牢) */
static inline int is_pool_image(const image *img) {
    return (img >= g_img_pool && img < g_img_pool + IMG_POOL_SIZE);
}

void image_free(image *img) {
    if (!img) return;
    if (is_pool_image(img)) return;
    free(img->data);
    free(img);
}

int image_load(const char *path, image *img) {
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    if (!data) {

        return -1;
    }
    if (!is_pool_image(img))
        free(img->data);
    img->width = w;
    img->height = h;
    img->channels = 4;
    img->data = data;
    return 0;
}

/* ---------- 描画プリミティブ ---------- */

void render_fill(image *img, uint8_t r, uint8_t g, uint8_t b) {
    size_t total = (size_t)img->width * img->height * 4;
    uint8_t *p = img->data;
    size_t i = 0;

    __m128i v16 = _mm_setr_epi8(
        r,g,b,255, r,g,b,255, r,g,b,255, r,g,b,255
    );
    uint32_t v32 = rgba32(r, g, b, 255);

    /* アライメント調整 (15 byte 以下なので単純ループ) */
    while (i < total && ((uintptr_t)(p + i) & 15)) {
        *(uint32_t*)(p + i) = v32;
        i += 4;
    }

    /* 64 byte アンロール + アラインドストア */
    for (; i + 64 <= total; i += 64) {
        _mm_store_si128((__m128i*)(p + i),      v16);
        _mm_store_si128((__m128i*)(p + i + 16), v16);
        _mm_store_si128((__m128i*)(p + i + 32), v16);
        _mm_store_si128((__m128i*)(p + i + 48), v16);
    }
    /* 16 byte 残り */
    for (; i + 16 <= total; i += 16) {
        _mm_store_si128((__m128i*)(p + i), v16);
    }
    /* 端数 */
    for (; i < total; i += 4) {
        *(uint32_t*)(p + i) = v32;
    }
}

void render_rect(image *img, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b) {
    int x0 = x, y0 = y;
    int x1 = x + w, y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img->width)  x1 = img->width;
    if (y1 > img->height) y1 = img->height;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t color = rgba32(r, g, b, 255);
    int stride = img->width * 4;
    int row_bytes = (x1 - x0) * 4;
    uint8_t *row = img->data + y0 * stride + x0 * 4;

    for (int py = y0; py < y1; py++) {
        uint32_t *p = (uint32_t*)row;
        int n = x1 - x0;
        /* 4ピクセル(16B)ずつ */
        int n4 = n >> 2;
        for (int i = 0; i < n4; i++) {
            p[0] = color; p[1] = color; p[2] = color; p[3] = color;
            p += 4;
        }
        n &= 3;
        for (int i = 0; i < n; i++) {
            *p++ = color;
        }
        row += stride;
    }
}

void render_circle_progress(image *img, int cx, int cy, int radius,
                            float progress, uint8_t r, uint8_t g, uint8_t b) {
    if (progress <= 0.0f) return;
    if (progress > 1.0f) progress = 1.0f;

    float end_angle = -M_PI_2 + 2.0f * M_PI * progress;

    int r2 = radius * radius;
    int inner = radius - 3;
    if (inner < 0) inner = 0;
    int inner2 = inner * inner;

    int y0 = cy - radius;
    int y1 = cy + radius;
    if (y0 < 0) y0 = 0;
    if (y1 >= img->height) y1 = img->height - 1;

    uint32_t color = rgba32(r, g, b, 255);

    for (int py = y0; py <= y1; py++) {
        int dy = py - cy;
        int dy2 = dy * dy;
        int dx = (int)sqrtf((float)(r2 - dy2));
        int x0 = cx - dx;
        int x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= img->width) x1 = img->width - 1;

        for (int px = x0; px <= x1; px++) {
            int ddx = px - cx;
            int dist2 = ddx * ddx + dy2;
            if (dist2 > r2 || dist2 < inner2) continue;

            float angle = atan2f((float)dy, (float)ddx);
            /* [-pi,pi] -> [-pi/2, 3pi/2] に正規化 */
            if (angle < -M_PI_2) angle += 2.0f * M_PI;
            if (angle <= end_angle) {
                uint8_t *p = img->data + ((size_t)py * img->width + px) * 4;
                *(uint32_t*)p = color;
            }
        }
    }
}

void render_text(image *img, const char *text, int x, int y,
                 uint8_t r, uint8_t g, uint8_t b) {
    font_draw(img, text, x, y, r, g, b, 16.0f);
}

/* ---------- フォント ---------- */

static font_info g_font;
static unsigned char *g_font_data = NULL;
static int g_font_loaded = 0;

/* Glyph cache */
#define GLYPH_CACHE_SIZE 64
typedef struct {
    int key;
    unsigned char *bitmap;
    int w, h, xoff, yoff, adv;
} glyph_cache_entry;

static glyph_cache_entry g_glyph_cache[GLYPH_CACHE_SIZE];
static int g_glyph_cache_idx = 0;

static glyph_cache_entry *glyph_cache_get(int codepoint, float scale, int *adv) {
    /* key: codepoint + size識別 (scale*200 は誤差を含むので size ベースが理想だが
       呼び出し側互換のため scale をそのまま使う) */
    int key = (codepoint << 8) | ((int)(scale * 200.0f) & 0xFF);
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (g_glyph_cache[i].key == key) {
            *adv = g_glyph_cache[i].adv;
            return &g_glyph_cache[i];
        }
    }
    int idx = g_glyph_cache_idx++ % GLYPH_CACHE_SIZE;
    glyph_cache_entry *e = &g_glyph_cache[idx];
    if (e->bitmap) stbtt_FreeBitmap(e->bitmap, NULL);
    e->key = key;
    int lsb;
    stbtt_GetCodepointHMetrics(&g_font, codepoint, &e->adv, &lsb);
    e->bitmap = stbtt_GetCodepointBitmap(&g_font, scale, scale,
                                          codepoint, &e->w, &e->h,
                                          &e->xoff, &e->yoff);
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

    if (sz > 4 && g_font_data[0] == 't' && g_font_data[1] == 't'
               && g_font_data[2] == 'c' && g_font_data[3] == 0x01) {
        unsigned major = (unsigned)(g_font_data[4])
                       | ((unsigned)(g_font_data[5]) << 8);
        unsigned num_fonts = (unsigned)(g_font_data[8])
                           | ((unsigned)(g_font_data[9]) << 8)
                           | ((unsigned)(g_font_data[10]) << 16)
                           | ((unsigned)(g_font_data[11]) << 24);
        unsigned offset_base = (major >= 2) ? 16 : 12;
        long long offset;
        if (num_fonts > 0) {
            if (major >= 2) {
                offset = (long long)(g_font_data[offset_base])
                       | ((long long)(g_font_data[offset_base+1]) << 8)
                       | ((long long)(g_font_data[offset_base+2]) << 16)
                       | ((long long)(g_font_data[offset_base+3]) << 24)
                       | ((long long)(g_font_data[offset_base+4]) << 32)
                       | ((long long)(g_font_data[offset_base+5]) << 40)
                       | ((long long)(g_font_data[offset_base+6]) << 48)
                       | ((long long)(g_font_data[offset_base+7]) << 56);
            } else {
                offset = (unsigned)(g_font_data[offset_base])
                       | ((unsigned)(g_font_data[offset_base+1]) << 8)
                       | ((unsigned)(g_font_data[offset_base+2]) << 16)
                       | ((unsigned)(g_font_data[offset_base+3]) << 24);
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
        return len * (int)(size * 0.6f + 2.0f);
    }

    /* 同一ポインタ & 同一サイズ の連続呼び出しをキャッシュ
       (動的文字列の場合はヒットしない点に注意) */
    static const char *prev_text = NULL;
    static float prev_size = 0.0f;
    static int prev_result = 0;
    if (text == prev_text && size == prev_size) return prev_result;
    prev_text = text;
    prev_size = size;

    float scale = stbtt_ScaleForPixelHeight(&g_font, size);
    int total = 0;
    for (const char *p = text; *p; ) {
        unsigned cp;
        p = decode_utf8(p, &cp);
        int adv;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, NULL);
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
        unsigned cp;
        p = decode_utf8(p, &cp);

        int adv;
        glyph_cache_entry *gce = glyph_cache_get((int)cp, scale, &adv);
        if (!gce || !gce->bitmap) {
            bx += (int)(adv * scale);
            continue;
        }

        int ox = bx + gce->xoff;
        int oy = baseline + gce->yoff;
        int gw = gce->w, gh = gce->h;

        int row0 = 0, row1 = gh;
        int col0 = 0, col1 = gw;
        if (oy < 0) row0 = -oy;
        if (oy + gh > img->height) row1 = img->height - oy;
        if (ox < 0) col0 = -ox;
        if (ox + gw > img->width) col1 = img->width - ox;
        if (row0 >= row1 || col0 >= col1) {
            bx += (int)(adv * scale);
            continue;
        }

        for (int row = row0; row < row1; row++) {
            int py = oy + row;
            uint8_t *dst_row = img->data + ((size_t)py * img->width + (ox + col0)) * 4;
            const unsigned char *src_row = gce->bitmap + row * gw + col0;
            int n = col1 - col0;

            for (int i = 0; i < n; i++) {
                unsigned char a = src_row[i];
                if (a == 0) {
                    dst_row += 4;
                    continue;
                }
                if (a == 255) {
                    dst_row[0] = r;
                    dst_row[1] = g;
                    dst_row[2] = b;
                    dst_row[3] = 255;
                } else {
                    unsigned ia = 255 - a;
                    unsigned tr = dst_row[0] * ia + r * a;
                    unsigned tg = dst_row[1] * ia + g * a;
                    unsigned tb = dst_row[2] * ia + b * a;
                    dst_row[0] = div255(tr);
                    dst_row[1] = div255(tg);
                    dst_row[2] = div255(tb);
                    dst_row[3] = 255;
                }
                dst_row += 4;
            }
        }
        bx += (int)(adv * scale);
    }
}