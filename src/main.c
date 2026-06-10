#include "common.h"
#include "log.h"
#include "json.h"
#include "config.h"
#include "http.h"
#include "oauth.h"
#include "token.h"
#include "device.h"
#include "render.h"
#include "auto_fix.h"
#include "twitch.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <ctype.h>
#include <curl/curl.h>
#include <emmintrin.h>
#include <spawn.h>

extern char **environ;

/* ================================================================
 * Constants
 * ================================================================ */
#define MAX_KEYS 15
#define MAX_TWITCH_KEYS 14
#define SCROLL_IV 0.033f
#define FETCH_IV 3
#define IDLE_TIMEOUT 60.0
#define W 72
#define H 72
#define MAX_EMOTES 6
#define MAX_TEXTS 13
#define MAX_NEXT  5
#define MAX_FONTS 5
#define MAX_FOLLOWS 100
#define MAX_STACK 32
#define MAX_LRU 50

/* ================================================================
 * Pages
 * ================================================================ */
enum page {
    PAGE_HOME, PAGE_TW, PAGE_LV, PAGE_TX, PAGE_NX,
    PAGE_ST, PAGE_SD, PAGE_OA, PAGE_FN, PAGE_UI
};

/* ================================================================
 * Scroll mode
 * ================================================================ */
enum scroll_mode { SCROLL_TITLE, SCROLL_CATEGORY };

/* ================================================================
 * Global State
 * ================================================================ */
static device *g_dev = NULL;
static enum page g_page = PAGE_HOME;
static char g_live[MAX_STR] = {0};
static int g_brightness = 50;
extern bool debug_mode;
#define g_debug_mode debug_mode

typedef struct { enum page pg; char ctx[MAX_STR]; } stack_entry;
static stack_entry g_stack[MAX_STACK];
static int g_stack_len = 0;

/* Twitch state */
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_followed[MAX_FOLLOWS];
static int g_followed_len = 0;
static json_val *g_lu[MAX_FOLLOWS];
static char g_id2lg_keys[MAX_FOLLOWS][MAX_STR];
static char g_id2lg_vals[MAX_FOLLOWS][MAX_STR];
static int g_id2lg_len = 0;
static char *g_tw_order[MAX_TWITCH_KEYS];
static int g_tw_order_len = 0;
static char g_titles[MAX_FOLLOWS][256];
static float g_title_ofs[MAX_FOLLOWS];
static float g_title_step[MAX_FOLLOWS];
static float g_title_w[MAX_FOLLOWS];
static bool g_title_wrapped[MAX_FOLLOWS];
static char g_categories[MAX_FOLLOWS][256];
static float g_cat_ofs[MAX_FOLLOWS];
static float g_cat_step[MAX_FOLLOWS];
static float g_cat_w[MAX_FOLLOWS];
static bool g_cat_wrapped[MAX_FOLLOWS];
static int g_views[MAX_FOLLOWS];
static double g_started_at[MAX_FOLLOWS];
static enum scroll_mode g_scroll_mode = SCROLL_TITLE;
static int g_last_online_count = 0;
static bool g_prev_online[MAX_FOLLOWS];
static int g_prev_online_len = 0;

/* LRU cache for profile images */
typedef struct { char key[MAX_STR]; image *img; } lru_entry;
static lru_entry g_lru_cache[MAX_LRU];
static int g_lru_len = 0;
static int g_lru_keys[MAX_LRU];

static char *g_emotes[] = {
    "BloodTrail", "HeyGuys", "LUL", "DinoDance", "HungryPaimon", "GlitchCat"
};
static char *g_default_texts[] = {
    "うおw", "うま", "うっま", "あ", "www", "wwww", "wwwww",
    "wwwww", "こっから勝・つ・ぞ！オイ！", "んん〜まかｧｧ",
    "うおおおおおおおおお", "きたあああああああ", "いいね"
};
static char *g_default_next[] = {"あ）"};

/* IRC */
static int g_irc_sock = -1;
static bool g_irc_joined[256];
static int g_irc_joined_len = 0;
static char g_irc_username[MAX_STR] = {0};
static int g_irc_username_tries = 0;
static time_t g_last_ping = 0;

static time_t g_last_input = 0;
static const char *g_cache_dir = NULL;

static char g_cid[MAX_STR], g_cs[MAX_STR], g_at[MAX_STR], g_rt[MAX_STR], g_uid[MAX_STR];

/* ================================================================
 * Helpers
 * ================================================================ */
static inline void safe_strcpy(char *dst, size_t size, const char *src) {
    if (!src || size == 0) return;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ================================================================
 * Cache helpers
 * ================================================================ */
static void ensure_cache_dir(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/.cache/streamdeck-twitch", home);
    mkdir(buf, 0755);
    g_cache_dir = buf;
}

static void cache_path(const char *name, char *out, size_t sz) {
    ensure_cache_dir();
    snprintf(out, sz, "%s/%s", g_cache_dir, name);
}

/* ================================================================
 * LRU cache
 * ================================================================ */
static int lru_find(const char *key) {
    for (int i = 0; i < g_lru_len; i++)
        if (strcmp(g_lru_cache[i].key, key) == 0) return i;
    return -1;
}

static void lru_touch(int idx) {
    int tmp = g_lru_keys[idx];
    for (int i = idx; i > 0; i--) g_lru_keys[i] = g_lru_keys[i-1];
    g_lru_keys[0] = tmp;
}

static image *lru_get(const char *key) {
    int idx = lru_find(key);
    if (idx < 0) return NULL;
    lru_touch(idx);
    return g_lru_cache[idx].img;
}

static void lru_set(const char *key, image *img) {
    int idx = lru_find(key);
    if (idx >= 0) {
        image_free(g_lru_cache[idx].img);
        g_lru_cache[idx].img = img;
        lru_touch(idx);
        return;
    }
    if (g_lru_len >= MAX_LRU) {
        int evict = g_lru_keys[MAX_LRU-1];
        image_free(g_lru_cache[evict].img);
        g_lru_cache[evict].img = img;
        safe_strcpy(g_lru_cache[evict].key, sizeof(g_lru_cache[evict].key), key);
        for (int i = MAX_LRU-1; i > 0; i--) g_lru_keys[i] = g_lru_keys[i-1];
        g_lru_keys[0] = evict;
        return;
    }
    int n = g_lru_len++;
    safe_strcpy(g_lru_cache[n].key, sizeof(g_lru_cache[n].key), key);
    g_lru_cache[n].img = img;
    for (int i = n; i > 0; i--) g_lru_keys[i] = g_lru_keys[i-1];
    g_lru_keys[0] = n;
}

/* ================================================================
 * Image helpers
 * ================================================================ */
static image *resize72(image *src) {
    image *dst = image_create(72, 72);
    if (!dst) return NULL;
    int sx_mul = (src->width  << 16) / 72;
    int sy_mul = (src->height << 16) / 72;
    for (int y = 0; y < 72; y++) {
        int sy = (y * sy_mul) >> 16;
        const uint8_t *src_row = src->data + (size_t)sy * src->width * 4;
        uint8_t *dst_row = dst->data + (size_t)y * 72 * 4;
        for (int x = 0; x < 72; x++) {
            int sx = (x * sx_mul) >> 16;
            *(uint32_t*)(dst_row + x * 4) = *(uint32_t*)(src_row + sx * 4);
        }
    }
    return dst;
}

static image *fetch_prof(const char *url) {
    if (!url || !*url) return NULL;

    image *cached = lru_get(url);
    if (cached) return cached;

    char path[1024];
    char hash[64];
    unsigned long h = 5381;
    for (const char *p = url; *p; p++) h = h * 33 + (unsigned char)*p;
    snprintf(hash, sizeof(hash), "%lx", h);
    cache_path(hash, path, sizeof(path));
    strncat(path, ".raw", sizeof(path)-strlen(path)-1);

    image *img = calloc(1, sizeof(image));
    if (image_load(path, img) == 0 && img->width > 0 && img->height > 0) {
        image *r = resize72(img);
        image_free(img);
        lru_set(url, r);
        return r;
    }
    image_free(img);

    http_resp *r = http_get(url, NULL);
    if (!r) return NULL;
    if (r->status != 200 || !r->body || r->body_len < 4) {
        http_resp_free(r);
        return NULL;
    }

    FILE *f = fopen(path, "wb");
    if (f) { fwrite(r->body, 1, r->body_len, f); fclose(f); }
    http_resp_free(r);

    img = calloc(1, sizeof(image));
    if (image_load(path, img) == 0 && img->width > 0 && img->height > 0) {
        image *r2 = resize72(img);
        image_free(img);
        lru_set(url, r2);
        return r2;
    }
    image_free(img);
    return NULL;
}

/* ================================================================
 * Text rendering (font)
 * ================================================================ */
static int measure_text(const char *text, int size) {
    return font_measure(text, (float)size);
}

static void draw_text(image *img, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b, int size) {
    int tw = font_measure(text, (float)size);
    int sx = x;
    if (tw < 72) sx = (72 - tw) / 2;
    font_draw(img, text, sx, y, r, g, b, (float)size);
}

static image *key_text_bg(const char *text, uint8_t br, uint8_t bg, uint8_t bb) {
    image *img = image_pool_get();
    render_fill(img, br, bg, bb);
    draw_text(img, 0, 0, text, 255, 255, 255, 13);
    return img;
}

/* ================================================================
 * Follows cache (簡易 JSON、シリアライズオーバーヘッド削減)
 * ================================================================ */
static void save_followed_cache(char **list, int len) {
    char path[1024];
    cache_path("followed.json", path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputc('[', f);
    for (int i = 0; i < len; i++) {
        if (i > 0) fputc(',', f);
        fprintf(f, "\"%s\"", list[i]);
    }
    fputs("]\n", f);
    fclose(f);
}

static char **load_followed_cache(int *out_len) {
    char path[1024];
    cache_path("followed.json", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *data = malloc((size_t)len+1);
    fread(data, 1, (size_t)len, f); fclose(f);
    data[len] = '\0';

    json_val *j = json_parse(data);
    free(data);
    if (!j || j->type != JSON_ARR) { json_free(j); return NULL; }

    char **out = calloc(j->arr.len, sizeof(char*));
    for (size_t i = 0; i < j->arr.len; i++) {
        if (j->arr.items[i].type == JSON_STR)
            out[i] = strdup(j->arr.items[i].str);
    }
    *out_len = (int)j->arr.len;
    json_free(j);
    return out;
}

/* ================================================================
 * Prev online state (簡易 JSON)
 * ================================================================ */
void save_prev_online(bool *online, int len) {
    char path[1024];
    cache_path("prev_online.json", path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;
    fputc('{', f);
    int first = 1;
    for (int i = 0; i < g_followed_len && i < len; i++) {
        if (!online[i] || !g_followed[i]) continue;
        if (!first) fputc(',', f);
        first = 0;
        fprintf(f, "\"%s\":true", g_followed[i]);
    }
    fputs("}\n", f);
    fclose(f);
}

static void load_prev_online(void) {
    char path[1024];
    cache_path("prev_online.json", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return; }
    char *data = malloc((size_t)len+1);
    fread(data, 1, (size_t)len, f); fclose(f);
    data[len] = '\0';

    json_val *j = json_parse(data);
    free(data);
    if (!j || j->type != JSON_OBJ) { json_free(j); return; }

    g_prev_online_len = 0;
    for (size_t i = 0; i < j->obj.len && g_prev_online_len < MAX_FOLLOWS; i++) {
        for (int k = 0; k < g_followed_len; k++) {
            if (strcmp(j->obj.pairs[i].key, g_followed[k]) == 0) {
                g_prev_online[g_prev_online_len++] = true;
                break;
            }
        }
    }
    json_free(j);
}

/* ================================================================
 * ID2LG helpers
 * ================================================================ */
static const char *id2lg_find(const char *id) {
    for (int i = 0; i < g_id2lg_len; i++)
        if (strcmp(g_id2lg_keys[i], id) == 0) return g_id2lg_vals[i];
    return NULL;
}

static void id2lg_set(const char *id, const char *lg) {
    for (int i = 0; i < g_id2lg_len; i++)
        if (strcmp(g_id2lg_keys[i], id) == 0) {
            safe_strcpy(g_id2lg_vals[i], sizeof(g_id2lg_vals[i]), lg);
            return;
        }
    if (g_id2lg_len >= MAX_FOLLOWS) return;
    safe_strcpy(g_id2lg_keys[g_id2lg_len], sizeof(g_id2lg_keys[g_id2lg_len]), id);
    safe_strcpy(g_id2lg_vals[g_id2lg_len], sizeof(g_id2lg_vals[g_id2lg_len]), lg);
    g_id2lg_len++;
}

/* ================================================================
 * lu helpers
 * ================================================================ */
static json_val *lu_get(const char *lg) {
    for (int i = 0; i < g_followed_len; i++)
        if (g_lu[i] && strcmp(g_followed[i], lg) == 0)
            return g_lu[i];
    return NULL;
}

/* ================================================================
 * Platform helpers (Linux) — system() 排除
 * ================================================================ */
static void platform_open_browser(const char *url) {
    pid_t pid;
    char *argv[] = {"xdg-open", (char*)url, NULL};
    posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ);
}

static void platform_speak_text(const char *text) {
    pid_t pid;
    char *argv[] = {"espeak-ng", "-v", "ja", (char*)text, NULL};
    posix_spawnp(&pid, "espeak-ng", NULL, NULL, argv, environ);
}

static json_val *twitch_api_call(const char *url) {
    http_resp *r = http_get(url, g_config.access_token);
    if (!r) { errorLog("twitch_api_call: http_get returned NULL"); return NULL; }
    if (r->status != 200) {
        errorLog("twitch_api_call: HTTP %d for %s", r->status, url);
        http_resp_free(r);
        return NULL;
    }
    json_val *j = json_parse(r->body);
    http_resp_free(r);
    return j;
}

static int token_refresh_full(void) {
    if (g_config.refresh_token[0] == '\0' || g_config.client_secret[0] == '\0')
        return -1;

    char body[4096];
    snprintf(body, sizeof(body),
        "grant_type=refresh_token"
        "&refresh_token=%s"
        "&client_id=%s"
        "&client_secret=%s",
        g_config.refresh_token, g_config.client_id, g_config.client_secret);

    http_resp *r = http_post_form("https://id.twitch.tv/oauth2/token", body, NULL);
    if (!r || r->status != 200) {
        errorLog("token refresh failed");
        http_resp_free(r);
        return -1;
    }

    json_val *j = json_parse(r->body);
    http_resp_free(r);
    if (!j) return -1;

    json_val *v;
    v = json_obj_get(j, "access_token");
    if (v && v->type == JSON_STR) {
        safe_strcpy(g_config.access_token, sizeof(g_config.access_token), v->str);
        safe_strcpy(g_at, sizeof(g_at), v->str);
    }
    v = json_obj_get(j, "refresh_token");
    if (v && v->type == JSON_STR) {
        safe_strcpy(g_config.refresh_token, sizeof(g_config.refresh_token), v->str);
        safe_strcpy(g_rt, sizeof(g_rt), v->str);
    }
    json_free(j);
    config_save(&g_config);
    infoLog("token refreshed");
    return 0;
}

static json_val *twitch_api_get(const char *base, const char *params) {
    char url[4096];
    if (params)
        snprintf(url, sizeof(url), "%s?%s", base, params);
    else
        snprintf(url, sizeof(url), "%s", base);

    json_val *j = twitch_api_call(url);
    if (!j) {
        if (g_config.refresh_token[0]) {
            infoLog("API call failed, trying token refresh");
            if (token_refresh_full() == 0)
                j = twitch_api_call(url);
        }
    }
    return j;
}

/* ================================================================
 * Fetch & render functions
 * ================================================================ */
static void draw_scroll_text(image *img, int y, const char *txt, float ofs,
                             uint8_t r, uint8_t g, uint8_t b, int tw) {
    if (tw <= 0) return;
    int xp = -(int)fmodf(ofs, (float)tw);
    draw_text(img, xp, y, txt, r, g, b, 14);
    if (xp + tw < 72)
        draw_text(img, xp + tw, y, txt, r, g, b, 14);
}

static const char *viewer_count_str(int v) {
    static char buf[32];
    if (v >= 1000)
        snprintf(buf, sizeof(buf), "%.1fk", (double)v / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

static int lu_find_idx(const char *lg) {
    for (int i = 0; i < g_followed_len; i++)
        if (g_followed[i] && strcmp(g_followed[i], lg) == 0) return i;
    return -1;
}

static image *tw_img(const char *prof_url, const char *login) {
    image *img = image_pool_get();

    image *prof = fetch_prof(prof_url);
    if (prof) {
        for (int y = 0; y < 72; y++) {
            memcpy(img->data + y * 288, prof->data + y * 288, 288);
        }
        for (int y = 0; y < 72; y++) {
            uint8_t *row = img->data + y * 288;
            for (int x = 0; x < 72; x++) row[x*4+3] = 255;
        }
    } else {
        render_fill(img, 30, 30, 30);
        draw_text(img, 4, 25, login, 255, 255, 255, 12);
    }

    int idx = lu_find_idx(login);
    if (idx < 0) return img;

    int v = 0;
    double st = 0;
    char txt[256] = {0};
    float ofs = 0;
    pthread_mutex_lock(&g_state_mu);
    v = g_views[idx];
    st = g_started_at[idx];
    if (g_scroll_mode == SCROLL_CATEGORY) {
        safe_strcpy(txt, sizeof(txt), g_categories[idx]);
        ofs = g_cat_ofs[idx];
    } else {
        safe_strcpy(txt, sizeof(txt), g_titles[idx]);
        ofs = g_title_ofs[idx];
    }
    pthread_mutex_unlock(&g_state_mu);

    if (v > 0) {
        const char *s = viewer_count_str(v);
        render_rect(img, 1, 1, 45, 14, 0, 0, 0);
        font_draw(img, s, 3, 1, 255, 255, 255, 10);
    }
    if (st > 0) {
        time_t now = time(NULL);
        double el = difftime(now, st);
        int h = (int)el / 3600;
        int m = ((int)el % 3600) / 60;
        char lab[32];
        if (h > 0)
            snprintf(lab, sizeof(lab), "%dh%dm", h, m);
        else
            snprintf(lab, sizeof(lab), "%dm", m);
        int tw = measure_text(lab, 11);
        int xo = 72 - tw - 3;
        render_rect(img, xo-1, 1, 71-xo+2, 14, 0, 0, 0);
        font_draw(img, lab, xo, 1, 255, 255, 255, 10);
    }

    if (txt[0]) {
        uint8_t tr = 255, tg = 217, tb = 0;
        int tw = 0;
        if (g_scroll_mode == SCROLL_CATEGORY) {
            tr = 200; tg = 245; tb = 255;
            tw = (int)g_cat_w[idx];
        } else {
            tw = (int)g_title_w[idx];
        }
        render_rect(img, 0, 72-21, 72, 21, 0, 0, 0);
        draw_scroll_text(img, 72-21, txt, ofs, tr, tg, tb, tw);
    }

    return img;
}

static void render_home(void) {
    if (!g_dev) return;
    image *img;
    img = key_text_bg("Twitch", 100, 0, 255);
    int r = device_set_key(g_dev, 0, img->data, (size_t)72*72*4); if (r < 0) errorLog("set_key 0 failed"); image_free(img);
    img = key_text_bg("OAuth", 0, 100, 200);
    r = device_set_key(g_dev, 1, img->data, (size_t)72*72*4); if (r < 0) errorLog("set_key 1 failed"); image_free(img);
    img = key_text_bg("Setting", 40, 40, 40);
    r = device_set_key(g_dev, 2, img->data, (size_t)72*72*4); if (r < 0) errorLog("set_key 2 failed"); image_free(img);
    for (int i = 3; i < MAX_KEYS; i++) {
        img = key_text_bg("", 0, 0, 0);
        r = device_set_key(g_dev, i, img->data, (size_t)72*72*4); if (r < 0) errorLog("set_key %d failed", i); image_free(img);
    }
}

static void render_tw(void) {
    if (!g_dev) return;
    static unsigned char prev_sha[MAX_KEYS][20] = {{0}};
    static int frame = 0;
    frame++;
    int capture = g_debug_mode && (frame % 30 == 0) ? 1 : 0;
    for (int i = 0; i < MAX_TWITCH_KEYS; i++) {
        image *img;
        if (i < g_tw_order_len && g_tw_order[i]) {
            char *lg = g_tw_order[i];
            json_val *u = lu_get(lg);
            if (u) {
                json_val *purl = json_obj_get(u, "profile_image_url");
                const char *prof = (purl && purl->type == JSON_STR) ? purl->str : "";
                img = tw_img(prof, lg);
            } else {
                img = key_text_bg(lg, 0, 0, 0);
            }
        } else {
            img = key_text_bg("", 0, 0, 0);
        }
        uint8_t *d = img->data;
        unsigned char h[20] = {0};
        __m128i x0 = _mm_setzero_si128(), x1 = _mm_setzero_si128();
        for (int j = 0; j < 72*72*4; j += 32) {
            __m128i v0 = _mm_loadu_si128((__m128i*)(d + j));
            __m128i v1 = _mm_loadu_si128((__m128i*)(d + j + 16));
            x0 = _mm_xor_si128(x0, v0);
            x1 = _mm_xor_si128(x1, v1);
        }
        _mm_storeu_si128((__m128i*)h, _mm_xor_si128(x0, x1));
        h[16] = d[0] ^ d[1]; h[17] = d[2] ^ d[3]; h[18] = d[4]; h[19] = d[5];
        if (memcmp(h, prev_sha[i], 20) != 0) {
            memcpy(prev_sha[i], h, 20);
            device_set_key(g_dev, i, d, (size_t)72*72*4);
        }
        if (capture) {
            char pname[80];
            snprintf(pname, sizeof(pname), "/tmp/streamdeck/btn%d.png", i);
            image_save_png(img, pname);
        }
        image_free(img);
    }
    image *img = key_text_bg("ホーム", 50, 0, 50);
    uint8_t *d = img->data;
    unsigned char h[20] = {0};
    __m128i x0 = _mm_setzero_si128(), x1 = _mm_setzero_si128();
    for (int j = 0; j < 72*72*4; j += 32) {
        __m128i v0 = _mm_loadu_si128((__m128i*)(d + j));
        __m128i v1 = _mm_loadu_si128((__m128i*)(d + j + 16));
        x0 = _mm_xor_si128(x0, v0);
        x1 = _mm_xor_si128(x1, v1);
    }
    _mm_storeu_si128((__m128i*)h, _mm_xor_si128(x0, x1));
    h[16] = d[0] ^ d[1]; h[17] = d[2] ^ d[3]; h[18] = d[4]; h[19] = d[5];
    if (memcmp(h, prev_sha[14], 20) != 0) {
        memcpy(prev_sha[14], h, 20);
        device_set_key(g_dev, 14, d, (size_t)72*72*4);
    }
    if (capture) {
        char pname[80];
        snprintf(pname, sizeof(pname), "/tmp/streamdeck/btn14.png");
        image_save_png(img, pname);
    }
    image_free(img);
}

static void render_lv(const char *lg) {
    (void)lg; if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    for (int i = 0; i < MAX_EMOTES; i++) {
        image *img = key_text_bg(g_emotes[i], 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img;
    img = key_text_bg("配信を見る", 20, 40, 20);
    device_set_key(g_dev, 11, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("TEXT", 20, 20, 40);
    device_set_key(g_dev, 12, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_tx(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    int n = MAX_TEXTS;
    for (int i = 0; i < n; i++) {
        image *img = key_text_bg(g_default_texts[i], 30, 30, 30);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img = key_text_bg("NEXT", 30, 0, 30);
    device_set_key(g_dev, 12, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_nx(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    for (int i = 0; i < 1; i++) {
        image *img = key_text_bg(g_default_next[i], 30, 30, 30);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_st(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img;
    img = key_text_bg("StreamDeck", 30, 30, 30);
    device_set_key(g_dev, 0, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("再起動", 60, 0, 0);
    device_set_key(g_dev, 1, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("Debug", 0, 60, 100);
    device_set_key(g_dev, 2, img->data, (size_t)72*72*4); image_free(img);
    for (int i = 3; i < 14; i++) {
        img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_sd(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img;
    img = key_text_bg("明るさUP", 40, 40, 0);
    device_set_key(g_dev, 0, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("明るさDW", 40, 0, 0);
    device_set_key(g_dev, 1, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_oa(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img = key_text_bg("Auth", 0, 100, 200);
    device_set_key(g_dev, 0, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("Back", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_fn(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    char *names[] = {"Noto Sans Bold", "Noto Sans Regular", "DejaVu Sans",
                     "Liberation Sans", "Liberation Serif"};
    for (int i = 0; i < 5 && i < 12; i++) {
        image *img = key_text_bg(names[i], 30, 30, 30);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

static void render_ui(void) {
    if (!g_dev) return;
    for (int i = 0; i < MAX_KEYS; i++) {
        image *img = key_text_bg("", 0, 0, 0);
        device_set_key(g_dev, i, img->data, (size_t)72*72*4); image_free(img);
    }
    image *img = key_text_bg("下部背景", 30, 30, 30);
    device_set_key(g_dev, 0, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("視聴数余白", 30, 30, 30);
    device_set_key(g_dev, 1, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("配信時間余白", 30, 30, 30);
    device_set_key(g_dev, 2, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("ホーム", 0, 40, 40);
    device_set_key(g_dev, 13, img->data, (size_t)72*72*4); image_free(img);
    img = key_text_bg("戻る", 40, 40, 0);
    device_set_key(g_dev, 14, img->data, (size_t)72*72*4); image_free(img);
}

/* ================================================================
 * IRC
 * ================================================================ */
static bool irc_connect(void) {
    if (g_at[0] == '\0' || g_irc_username[0] == '\0') return false;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &res) != 0) return false;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return false; }

    struct timeval tv = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    char buf[512];
    snprintf(buf, sizeof(buf), "PASS oauth:%s\r\n", g_at);
    write(sock, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "NICK %s\r\n", g_irc_username);
    write(sock, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "CAP REQ :twitch.tv/tags twitch.tv/commands twitch.tv/membership\r\n");
    write(sock, buf, strlen(buf));

    g_irc_sock = sock;
    g_irc_joined[0] = false;
    g_irc_joined_len = 0;
    g_last_ping = time(NULL);
    infoLog("IRC connected as %s", g_irc_username);
    return true;
}

static void irc_send(const char *channel, const char *msg) {
    if (g_irc_sock < 0) return;
    char buf[4096];
    snprintf(buf, sizeof(buf), "PRIVMSG #%s :%s\r\n", channel, msg);
    write(g_irc_sock, buf, strlen(buf));
    infoLog("IRC sent to #%s: %s", channel, msg);
}

static bool fetch_irc_username(void) {
    g_irc_username_tries++;
    json_val *j = twitch_api_get("https://api.twitch.tv/helix/users", NULL);
    if (!j) return false;
    json_val *data = json_obj_get(j, "data");
    if (!data || data->type != JSON_ARR || data->arr.len == 0) {
        json_free(j); return false;
    }
    json_val *user = json_arr_get(data, 0);
    json_val *v = json_obj_get(user, "login");
    if (v && v->type == JSON_STR) {
        safe_strcpy(g_irc_username, sizeof(g_irc_username), v->str);
        safe_strcpy(g_uid, sizeof(g_uid), v->str);
        safe_strcpy(g_config.user_id, sizeof(g_config.user_id), v->str);
        safe_strcpy(g_config.channel_name, sizeof(g_config.channel_name), v->str);
        config_save(&g_config);
        infoLog("IRC username: %s", g_irc_username);
        json_free(j);
        return true;
    }
    json_free(j);
    return false;
}

static void *irc_thread(void *arg) {
    (void)arg;
    while (1) {
        if (g_at[0] == '\0') { sleep(5); continue; }

        if (g_irc_username[0] == '\0') {
            if (!fetch_irc_username()) {
                safe_strcpy(g_irc_username, sizeof(g_irc_username), "justinfan12345");
            }
        }

        if (g_irc_sock < 0) {
            if (!irc_connect()) {
                sleep(5);
                continue;
            }
        }

        if (g_live[0] && !g_irc_joined[0]) {
            char buf[512];
            snprintf(buf, sizeof(buf), "JOIN #%s\r\n", g_live);
            write(g_irc_sock, buf, strlen(buf));
            g_irc_joined[0] = true;
            g_irc_joined_len = 1;
        }

        time_t now = time(NULL);
        if (difftime(now, g_last_ping) > 120) {
            write(g_irc_sock, "PING :tmi.twitch.tv\r\n", 21);
            g_last_ping = now;
        }

        struct pollfd pfd = {g_irc_sock, POLLIN, 0};
        int ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            int n = (int)read(g_irc_sock, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                if (strncmp(buf, "PING", 4) == 0) {
                    write(g_irc_sock, "PONG :tmi.twitch.tv\r\n", 21);
                }
            } else {
                close(g_irc_sock);
                g_irc_sock = -1;
                g_irc_joined[0] = false;
                g_irc_joined_len = 0;
            }
        }
    }
    return NULL;
}

/* ================================================================
 * Navigation / Show
 * ================================================================ */
static void push_stack(enum page pg, const char *ctx) {
    if (g_stack_len < MAX_STACK) {
        g_stack[g_stack_len].pg = pg;
        safe_strcpy(g_stack[g_stack_len].ctx, sizeof(g_stack[g_stack_len].ctx), ctx);
        g_stack_len++;
    }
}

static void page_show(enum page pg, const char *ctx, bool push) {
    if (push && pg != g_page) {
        push_stack(g_page, g_live);
    }
    g_page = pg;
    if (ctx) safe_strcpy(g_live, sizeof(g_live), ctx);
    else g_live[0] = '\0';

    if (g_dev) device_reset_cache(g_dev);

    switch (pg) {
        case PAGE_HOME: render_home(); break;
        case PAGE_TW:   render_tw(); break;
        case PAGE_LV:   render_lv(ctx); break;
        case PAGE_TX:   render_tx(); break;
        case PAGE_NX:   render_nx(); break;
        case PAGE_ST:   render_st(); break;
        case PAGE_SD:   render_sd(); break;
        case PAGE_OA:   render_oa(); break;
        case PAGE_FN:   render_fn(); break;
        case PAGE_UI:   render_ui(); break;
    }
}

static void page_back(void) {
    if (g_stack_len == 0) {
        page_show(PAGE_HOME, "", false);
        return;
    }
    g_stack_len--;
    page_show(g_stack[g_stack_len].pg, g_stack[g_stack_len].ctx, false);
}

/* ================================================================
 * Key handlers
 * ================================================================ */
static bool handle_nav(int k, bool home, bool back) {
    if (home && k == 13) { page_show(PAGE_HOME, "", false); return true; }
    if (back && k == 14) { page_back(); return true; }
    return false;
}

static void handle_home(int k) {
    switch (k) {
        case 0: page_show(PAGE_TW, "", true); break;
        case 1: page_show(PAGE_OA, "", true); break;
        case 2: page_show(PAGE_ST, "", true); break;
    }
}

static void handle_tw(int k) {
    if (handle_nav(k, false, true)) return;
    if (k < g_tw_order_len && g_tw_order[k])
        page_show(PAGE_LV, g_tw_order[k], true);
}

static void handle_lv(int k) {
    if (handle_nav(k, true, true)) return;
    if (k < MAX_EMOTES && g_live[0]) {
        irc_send(g_live, g_emotes[k]);
    } else if (k == 11 && g_live[0]) {
        char url[512];
        snprintf(url, sizeof(url), "https://www.twitch.tv/%s", g_live);
        platform_open_browser(url);
    } else if (k == 12 && g_live[0]) {
        page_show(PAGE_TX, g_live, true);
    }
}

static void handle_tx(int k) {
    if (handle_nav(k, true, true)) return;
    if (k < MAX_TEXTS && g_live[0])
        irc_send(g_live, g_default_texts[k]);
    else if (k == 12 && g_live[0])
        page_show(PAGE_NX, g_live, true);
}

static void handle_nx(int k) {
    if (handle_nav(k, true, true)) return;
    if (k < 1 && g_live[0])
        irc_send(g_live, g_default_next[k]);
}

static void handle_st(int k) {
    if (handle_nav(k, true, false)) return;
    if (k == 14) { page_show(PAGE_HOME, "", false); return; }
    switch (k) {
        case 0: page_show(PAGE_SD, "", true); break;
        case 1: {
            pid_t pid;
            char *argv[] = {"reboot", NULL};
            posix_spawnp(&pid, "reboot", NULL, NULL, argv, environ);
            break;
        }
        case 2:
            g_debug_mode = !g_debug_mode;
            infoLog("Debug mode %s", g_debug_mode ? "ON" : "OFF");
            render_st();
            break;
    }
}

static void handle_sd(int k) {
    if (handle_nav(k, true, true)) return;
    switch (k) {
        case 0:
            g_brightness = (g_brightness + 10 > 100) ? 100 : g_brightness + 10;
            device_set_brightness(g_dev, g_brightness);
            render_sd();
            break;
        case 1:
            g_brightness = (g_brightness - 10 < 0) ? 0 : g_brightness - 10;
            device_set_brightness(g_dev, g_brightness);
            render_sd();
            break;
    }
}

static void init_follows(void);
static void fetch_users(char **logins, int len);
static void start_oauth(void);

static void handle_oa(int k) {
    if (handle_nav(k, false, true)) return;
    if (k == 0) start_oauth();
}

static void start_oauth(void) {
    if (g_config.client_id[0] == '\0') {
        warnLog("Client ID not set. Set it in config first.");
        return;
    }
    if (g_config.client_secret[0] == '\0') {
        warnLog("Client Secret not set. Set it in config first.");
        return;
    }

    if (oauth_start_server() < 0) {
        errorLog("OAuth: %s", oauth_get_error());
        return;
    }

    if (oauth_open_browser(g_config.client_id) < 0) {
        errorLog("OAuth: %s", oauth_get_error());
        oauth_stop_server();
        return;
    }

    infoLog("Waiting for OAuth callback...");

    if (oauth_wait_for_result(60) < 0) {
        errorLog("OAuth: %s", oauth_get_error());
        oauth_stop_server();
        return;
    }

    token_info t;
    if (oauth_exchange_code(g_config.client_id, g_config.client_secret,
                            oauth_get_code(), &t) < 0) {
        errorLog("OAuth: %s", oauth_get_error());
        oauth_stop_server();
        return;
    }
    oauth_stop_server();

    safe_strcpy(g_config.access_token, sizeof(g_config.access_token), t.access_token);
    safe_strcpy(g_config.refresh_token, sizeof(g_config.refresh_token), t.refresh_token);
    safe_strcpy(g_at, sizeof(g_at), t.access_token);
    safe_strcpy(g_rt, sizeof(g_rt), t.refresh_token);
    config_save(&g_config);

    infoLog("OAuth successful! Token obtained.");

    json_val *user_j = twitch_api_get("https://api.twitch.tv/helix/users", NULL);
    if (user_j) {
        json_val *data = json_obj_get(user_j, "data");
        if (data && data->type == JSON_ARR && data->arr.len > 0) {
            json_val *u = json_arr_get(data, 0);
            json_val *v = json_obj_get(u, "id");
            if (v && v->type == JSON_STR) safe_strcpy(g_uid, sizeof(g_uid), v->str);
            v = json_obj_get(u, "login");
            if (v && v->type == JSON_STR) safe_strcpy(g_irc_username, sizeof(g_irc_username), v->str);
        }
        json_free(user_j);
    }

    if (g_page == PAGE_OA) {
        init_follows();
        fetch_users(g_followed, g_followed_len);
        page_show(PAGE_TW, "", false);
    }
}

static void handle_fn(int k) {
    if (handle_nav(k, true, true)) return;
    if (k >= 0 && k < 5) page_show(PAGE_HOME, "", false);
}

static void handle_ui(int k) {
    if (handle_nav(k, true, true)) return;
}

static void on_key(int k) {
    g_last_input = time(NULL);
    if (k < 0 || k >= MAX_KEYS) return;

    switch (g_page) {
        case PAGE_HOME: handle_home(k); break;
        case PAGE_TW:   handle_tw(k); break;
        case PAGE_LV:   handle_lv(k); break;
        case PAGE_TX:   handle_tx(k); break;
        case PAGE_NX:   handle_nx(k); break;
        case PAGE_ST:   handle_st(k); break;
        case PAGE_SD:   handle_sd(k); break;
        case PAGE_OA:   handle_oa(k); break;
        case PAGE_FN:   handle_fn(k); break;
        case PAGE_UI:   handle_ui(k); break;
    }
}

/* ================================================================
 * Background fetch / scroll
 * ================================================================ */
typedef struct { char lg[MAX_STR]; int viewers; double started; char title[256]; char game[256]; } stream_info;

static int cmp_stream(const void *a, const void *b) {
    const stream_info *sa = a;
    const stream_info *sb = b;
    if (sb->viewers > sa->viewers) return 1;
    if (sb->viewers < sa->viewers) return -1;
    return 0;
}

static void fetch_streams(void) {
    if (g_followed_len == 0) return;

    char url[8192] = "https://api.twitch.tv/helix/streams?";
    char *p = url + strlen(url);
    const char *url_end = url + sizeof(url) - 1;
    int first = 1;
    for (int i = 0; i < g_followed_len; i++) {
        json_val *u = lu_get(g_followed[i]);
        if (!u) continue;
        json_val *idv = json_obj_get(u, "id");
        if (!idv || idv->type != JSON_STR) continue;
        int n = snprintf(p, (size_t)(url_end - p), "%suser_id=%s", first ? "" : "&", idv->str);
        if (n > 0 && p + n < url_end) p += n;
        first = 0;
    }

    if (first) return;

    json_val *j = twitch_api_get(url, NULL);
    if (!j) return;

    json_val *data = json_obj_get(j, "data");
    if (!data || data->type != JSON_ARR) { json_free(j); return; }

    stream_info online[64];
    int online_len = 0;

    for (size_t i = 0; i < data->arr.len && online_len < 64; i++) {
        json_val *s = json_arr_get(data, i);
        json_val *uidv = json_obj_get(s, "user_id");
        if (!uidv || uidv->type != JSON_STR) continue;
        const char *lg = id2lg_find(uidv->str);
        if (!lg) continue;

        int dup = 0;
        for (int d = 0; d < online_len; d++) {
            if (strcmp(online[d].lg, lg) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        safe_strcpy(online[online_len].lg, sizeof(online[0].lg), lg);
        json_val *vv = json_obj_get(s, "viewer_count");
        online[online_len].viewers = (vv && vv->type == JSON_NUM) ? (int)vv->num : 0;

        json_val *sv = json_obj_get(s, "started_at");
        if (sv && sv->type == JSON_STR) {
            struct tm tm = {0};
            char *r = strptime(sv->str, "%Y-%m-%dT%H:%M:%S", &tm);
            if (r) online[online_len].started = (double)mktime(&tm);
            else online[online_len].started = 0;
        } else online[online_len].started = 0;

        json_val *tv = json_obj_get(s, "title");
        if (tv && tv->type == JSON_STR) safe_strcpy(online[online_len].title, sizeof(online[0].title), tv->str);
        else online[online_len].title[0] = '\0';

        json_val *gv = json_obj_get(s, "game_name");
        if (gv && gv->type == JSON_STR) safe_strcpy(online[online_len].game, sizeof(online[0].game), gv->str);
        else online[online_len].game[0] = '\0';

        online_len++;
    }

    qsort(online, (size_t)online_len, sizeof(stream_info), cmp_stream);

    if (online_len > MAX_TWITCH_KEYS) online_len = MAX_TWITCH_KEYS;

    pthread_mutex_lock(&g_state_mu);
    g_tw_order_len = 0;
    memset(g_tw_order, 0, sizeof(g_tw_order));
    memset(g_views, 0, sizeof(g_views));
    memset(g_started_at, 0, sizeof(g_started_at));
    memset(g_titles, 0, sizeof(g_titles));
    memset(g_categories, 0, sizeof(g_categories));

    bool current_online[MAX_FOLLOWS] = {false};

    for (int i = 0; i < online_len; i++) {
        int idx = lu_find_idx(online[i].lg);
        if (idx < 0) continue;

        g_tw_order[i] = g_followed[idx];
        g_tw_order_len = i+1;
        g_views[idx] = online[i].viewers;
        g_started_at[idx] = online[i].started;
        safe_strcpy(g_titles[idx], sizeof(g_titles[0]), online[i].title);
        safe_strcpy(g_categories[idx], sizeof(g_categories[0]), online[i].game);
        g_title_w[idx] = (float)measure_text(online[i].title, 14);
        int tlen = (int)strlen(online[i].title);
        g_title_step[idx] = (g_title_w[idx] / fmaxf(2.0f, fminf(8.0f, (float)tlen * 0.2f + 1.5f))) * SCROLL_IV;
        g_cat_w[idx] = (float)measure_text(online[i].game, 14);
        int clen = (int)strlen(online[i].game);
        g_cat_step[idx] = (g_cat_w[idx] / fmaxf(2.0f, fminf(8.0f, (float)clen * 0.2f + 1.5f))) * SCROLL_IV;
        current_online[idx] = true;
    }

    for (int i = 0; i < g_followed_len && i < MAX_FOLLOWS; i++) {
        if (current_online[i] && !g_prev_online[i]) {
            infoLog("%s started streaming!", g_followed[i]);
            char msg[256];
            snprintf(msg, sizeof(msg), "%s streaming", g_followed[i]);
            platform_speak_text(msg);
        }
    }
    memcpy(g_prev_online, current_online, sizeof(bool) * (size_t)g_followed_len);
    g_prev_online_len = g_followed_len;
    save_prev_online(g_prev_online, g_followed_len);

    if (online_len != g_last_online_count) {
        infoLog("%d streams online", online_len);
        g_last_online_count = online_len;
    }
    pthread_mutex_unlock(&g_state_mu);
    json_free(j);
}

static bool scroll_all(void) {
    bool changed = false;
    pthread_mutex_lock(&g_state_mu);

    if (g_scroll_mode == SCROLL_TITLE) {
        bool all_done = true;
        for (int i = 0; i < g_tw_order_len; i++) {
            int idx = lu_find_idx(g_tw_order[i]);
            if (idx < 0) continue;
            float prev = g_title_ofs[idx];
            g_title_ofs[idx] += g_title_step[idx];
            if (g_title_w[idx] > 0 &&
                fmodf(g_title_ofs[idx], g_title_w[idx]) + g_title_step[idx] >= g_title_w[idx])
                g_title_wrapped[idx] = true;
            if (!g_title_wrapped[idx]) all_done = false;
            if (g_title_ofs[idx] != prev) changed = true;
        }
        if (all_done && g_tw_order_len > 0) {
            g_scroll_mode = SCROLL_CATEGORY;
            for (int i = 0; i < g_tw_order_len; i++) {
                int idx = lu_find_idx(g_tw_order[i]);
                if (idx < 0) continue;
                g_cat_ofs[idx] = 0;
                g_cat_wrapped[idx] = false;
            }
            changed = true;
        }
    } else if (g_scroll_mode == SCROLL_CATEGORY) {
        bool all_done = true;
        for (int i = 0; i < g_tw_order_len; i++) {
            int idx = lu_find_idx(g_tw_order[i]);
            if (idx < 0) continue;
            float prev = g_cat_ofs[idx];
            g_cat_ofs[idx] += g_cat_step[idx];
            if (g_cat_w[idx] > 0 &&
                fmodf(g_cat_ofs[idx], g_cat_w[idx]) + g_cat_step[idx] >= g_cat_w[idx])
                g_cat_wrapped[idx] = true;
            if (!g_cat_wrapped[idx]) all_done = false;
            if (g_cat_ofs[idx] != prev) changed = true;
        }
        if (all_done && g_tw_order_len > 0) {
            g_scroll_mode = SCROLL_TITLE;
            for (int i = 0; i < g_tw_order_len; i++) {
                int idx = lu_find_idx(g_tw_order[i]);
                if (idx < 0) continue;
                g_title_ofs[idx] = 0;
                g_title_wrapped[idx] = false;
            }
            changed = true;
        }
    }

    pthread_mutex_unlock(&g_state_mu);
    return changed;
}

/* ================================================================
 * Init functions
 * ================================================================ */
static void init_follows(void) {
    char **cached = load_followed_cache(&g_followed_len);
    if (cached) {
        for (int i = 0; i < g_followed_len; i++) {
            g_followed[i] = cached[i];
            g_lu[i] = NULL;
        }
        free(cached);
        infoLog("follows loaded from cache: %d", g_followed_len);
        return;
    }

    if (g_uid[0] && g_config.access_token[0]) {
        char url[4096];
        snprintf(url, sizeof(url),
            "https://api.twitch.tv/helix/channels/followed?user_id=%s&first=100", g_uid);
        json_val *j = twitch_api_get(url, NULL);
        if (j) {
            json_val *data = json_obj_get(j, "data");
            if (data && data->type == JSON_ARR) {
                g_followed_len = 0;
                for (size_t i = 0; i < data->arr.len && g_followed_len < MAX_FOLLOWS; i++) {
                    json_val *br = json_arr_get(data, i);
                    json_val *v = json_obj_get(br, "broadcaster_login");
                    if (v && v->type == JSON_STR) {
                        g_followed[g_followed_len] = strdup(v->str);
                        g_lu[g_followed_len] = NULL;
                        for (char *p = g_followed[g_followed_len]; *p; p++) *p = (char)tolower(*p);
                        g_followed_len++;
                    }
                }
                save_followed_cache(g_followed, g_followed_len);
            }
            json_free(j);
        }
    }

    if (g_followed_len == 0) {
        char *defaults[] = {"hanjoudesu", "bijusan", "oniyadayo", "dmf_kyochan",
            "vodkavdk", "lazvell", "ade3_3", "goroujp", "batora324",
            "kato_junichi0817", "crowfps__", "gon_vl", "yuyuta0702"};
        g_followed_len = 13;
        for (int i = 0; i < g_followed_len; i++) {
            g_followed[i] = strdup(defaults[i]);
            g_lu[i] = NULL;
        }
    }
}

static void fetch_users(char **logins, int len) {
    for (int i = 0; i < len; i += 100) {
        int batch_end = (i + 100 < len) ? i + 100 : len;
        char url[8192] = "https://api.twitch.tv/helix/users?";
        char *p = url + strlen(url);
        const char *url_end = url + sizeof(url) - 1;
        for (int j = i; j < batch_end; j++) {
            int n = snprintf(p, (size_t)(url_end - p), "login=%s&", logins[j]);
            if (n > 0 && p + n < url_end) p += n;
        }
        if (p > url && *(p-1) == '&') {
            *(p-1) = '\0';
        } else {
            *p = '\0';
        }

        json_val *j = twitch_api_get(url, NULL);
        if (!j) continue;
        json_val *data = json_obj_get(j, "data");
        if (data && data->type == JSON_ARR) {
            for (size_t k = 0; k < data->arr.len; k++) {
                json_val *u = json_arr_get(data, k);
                json_val *lv = json_obj_get(u, "login");
                if (!lv || lv->type != JSON_STR) continue;
                const char *login = lv->str;

                for (int f = 0; f < g_followed_len; f++) {
                    if (g_followed[f] && strcasecmp(g_followed[f], login) == 0) {
                        json_val *copy = json_clone(u);
                        json_free(g_lu[f]);
                        g_lu[f] = copy;
                        break;
                    }
                }
                json_val *idv = json_obj_get(u, "id");
                if (idv && idv->type == JSON_STR)
                    id2lg_set(idv->str, login);
            }
        }
        json_free(j);
    }
}

/* ================================================================
 * Main loop
 * ================================================================ */
static void main_loop(void) {
    time_t last_fetch = 0;
    uint8_t input_buf[32];
    uint8_t prev_keys[MAX_KEYS] = {0};

    if (g_dev && g_page == PAGE_TW) render_tw();

    while (1) {
        time_t now = time(NULL);
        bool force_render = false;

        if (difftime(now, last_fetch) >= FETCH_IV) {
            last_fetch = now;
            fetch_streams();
            force_render = true;
        }

        bool scrolled = scroll_all();
        if ((scrolled || force_render) && g_page == PAGE_TW) render_tw();

        if (g_page != PAGE_TW && g_page != PAGE_LV &&
            g_page != PAGE_TX && g_page != PAGE_NX &&
            difftime(now, g_last_input) >= IDLE_TIMEOUT) {
            page_show(PAGE_TW, "", false);
        }

        if (g_dev) {
            int ret = device_read_input(g_dev, input_buf, sizeof(input_buf));
            if (ret >= 4 && input_buf[0] == 0x01) {
                for (int i = 0; i < MAX_KEYS && i+4 < ret; i++) {
                    uint8_t cur = input_buf[4+i];
                    if (cur != prev_keys[i]) {
                        prev_keys[i] = cur;
                        if (cur == 1) on_key(i);
                    }
                }
            }
        }

        usleep(16000);
    }
}

/* ================================================================
 * Entry point
 * ================================================================ */
int main(int argc, char **argv) {
    log_init();
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));

    ensure_cache_dir();

    if (argc > 1 && strcmp(argv[1], "--auto-fix") == 0) {
        auto_fix_run(argc, argv);
        log_close();
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--virtual") == 0) {
        infoLog("virtual mode (not yet implemented in C)");
        log_close();
        return 0;
    }

    config_defaults(&g_config);
    if (config_load(&g_config) < 0) {
        infoLog("no config found, running initial setup");

        infoLog("Enter Twitch Client ID:");
        if (fgets(g_config.client_id, MAX_STR, stdin))
            g_config.client_id[strcspn(g_config.client_id, "\n")] = 0;
        infoLog("Enter Twitch Client Secret:");
        if (fgets(g_config.client_secret, MAX_STR, stdin))
            g_config.client_secret[strcspn(g_config.client_secret, "\n")] = 0;
        config_save(&g_config);
    }

    safe_strcpy(g_cid, sizeof(g_cid), g_config.client_id);
    safe_strcpy(g_cs, sizeof(g_cs), g_config.client_secret);
    safe_strcpy(g_at, sizeof(g_at), g_config.access_token);
    safe_strcpy(g_rt, sizeof(g_rt), g_config.refresh_token);
    safe_strcpy(g_uid, sizeof(g_uid), g_config.user_id);

    char cid_hdr[2048];
    snprintf(cid_hdr, sizeof(cid_hdr), "Client-ID: %s", g_config.client_id);
    http_set_extra_header(cid_hdr);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *font_paths[] = {
        "/usr/share/fonts/OTF/ipagp.ttf",
        "/usr/share/fonts/OTF/ipag.ttf",
        "/usr/share/fonts/OTF/ipam.ttf",
        "/usr/share/fonts/OTF/ipamp.ttf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
    };
    int font_ok = 0;
    for (size_t i = 0; i < sizeof(font_paths)/sizeof(font_paths[0]); i++) {
        if (font_init(font_paths[i]) == 0) { font_ok = 1; break; }
    }
    if (!font_ok) warnLog("No usable font found. Text will be blank.");

    g_dev = device_open();
    if (!g_dev) {
        warnLog("no Stream Deck device found, exiting");
        log_close();
        return 1;
    }

    device_set_brightness(g_dev, g_brightness);
    g_last_input = time(NULL);

    if (g_at[0] == '\0') {
        page_show(PAGE_HOME, "", false);
    } else {
        init_follows();
        fetch_users(g_followed, g_followed_len);
        load_prev_online();
        page_show(PAGE_TW, "", false);
    }

    pthread_t irc_tid;
    pthread_create(&irc_tid, NULL, irc_thread, NULL);
    pthread_detach(irc_tid);

    infoLog("StreamDeck C started");

    main_loop();

    device_close(g_dev);
    http_close_all();
    log_close();
    return 0;
}