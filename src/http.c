#include "http.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>

/* ── 伸長バッファ（64 KB プリアロケートで realloc 回数を 0 に近づける） ── */
struct mem_buf {
    char *data;
    size_t len;
    size_t cap;
};

static void mb_init(struct mem_buf *mb, size_t initial_cap) {
    mb->cap = initial_cap ? initial_cap : 4096;
    mb->data = (char *)malloc(mb->cap);
    if (mb->data) mb->data[0] = '\0';
    mb->len = 0;
}

static void mb_free(struct mem_buf *mb) {
    free(mb->data);
    mb->data = NULL;
    mb->len = mb->cap = 0;
}

static void mb_append(struct mem_buf *mb, const char *ptr, size_t sz) {
    if (!mb->data) return;
    if (mb->len + sz + 1 > mb->cap) {
        while (mb->len + sz + 1 > mb->cap) mb->cap <<= 1;
        char *n = (char *)realloc(mb->data, mb->cap);
        if (!n) { free(mb->data); mb->data = NULL; return; }
        mb->data = n;
    }
    memcpy(mb->data + mb->len, ptr, sz);
    mb->len += sz;
    mb->data[mb->len] = '\0';
}

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    size_t total = size * nmemb;
    struct mem_buf *mb = (struct mem_buf *)user;
    mb_append(mb, (const char *)ptr, total);
    return total;
}

/* ── グローバル状態（既存 API 互換） ── */
static char g_extra_header[2048] = {0};

static CURL *curl_easy_new(void) {
    static int global_init = 0;
    if (!global_init) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        global_init = 1;
    }
    return curl_easy_init();
}

static http_resp *do_curl_req(const char *url, const char *post_body,
                              const char *bearer, bool is_form) {
    CURL *c = curl_easy_new();
    if (!c) {
        http_resp *r = (http_resp *)calloc(1, sizeof(http_resp));
        r->status = -1;
        return r;
    }

    struct mem_buf mb = {0};
    mb_init(&mb, 65536);  /* 多くの API レスポンスは 64 KB 以内で収まり realloc 不要 */

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: streamdeck-twitch/1.0");

    if (g_extra_header[0]) {
        headers = curl_slist_append(headers, g_extra_header);
    }

    if (bearer && bearer[0]) {
        char auth[2048];
        int n = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        if (n > 0 && (size_t)n < sizeof(auth))
            headers = curl_slist_append(headers, auth);
    }

    if (post_body) {
        if (is_form) {
            headers = curl_slist_append(headers,
                "Content-Type: application/x-www-form-urlencoded");
        } else {
            headers = curl_slist_append(headers, "Content-Type: application/json");
        }
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    }

    /* ── libcurl 最適化設定 ── */
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);   /* 接続再利用を最大化 */
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);        /* マルチスレッド安全 */
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* 自動圧縮解凍 (gzip/deflate) */

    CURLcode res = curl_easy_perform(c);
    http_resp *r = (http_resp *)calloc(1, sizeof(http_resp));

    if (res != CURLE_OK) {
        errorLog("HTTP %s failed: %s", post_body ? "POST" : "GET", curl_easy_strerror(res));
        r->status = -1;
        mb_free(&mb);
    } else {
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        r->status = (int)status;
        r->body = mb.data;
        r->body_len = mb.len;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return r;
}

/* ── 公開 API ── */
void http_set_extra_header(const char *hdr) {
    if (!hdr) {
        g_extra_header[0] = '\0';
        return;
    }
    size_t n = strlen(hdr);
    if (n >= sizeof(g_extra_header)) n = sizeof(g_extra_header) - 1;
    memcpy(g_extra_header, hdr, n);
    g_extra_header[n] = '\0';
}

void http_close_all(void) {
}

http_resp *http_get(const char *url, const char *bearer_token) {
    return do_curl_req(url, NULL, bearer_token, false);
}

http_resp *http_post(const char *url, const char *body, const char *bearer_token) {
    return do_curl_req(url, body, bearer_token, false);
}

http_resp *http_post_form(const char *url, const char *body, const char *bearer_token) {
    return do_curl_req(url, body, bearer_token, true);
}

void http_resp_free(http_resp *r) {
    if (!r) return;
    free(r->body);
    free(r);
}