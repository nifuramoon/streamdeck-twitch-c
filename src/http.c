#include "http.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <pthread.h>

/* ── 伸長バッファ（64 KB プリアロケート） ── */
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
    mb_append((struct mem_buf *)user, (const char *)ptr, total);
    return total;
}

/* ── グローバル curl ハンドル（接続再利用） ── */
static CURL *g_curl = NULL;
static pthread_mutex_t g_curl_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_extra_header[2048] = {0};

static CURL *curl_get_handle(void) {
    if (g_curl) return g_curl;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (!g_curl) return NULL;

    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(g_curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(g_curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPIDLE, 120L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(g_curl, CURLOPT_ACCEPT_ENCODING, "");
    return g_curl;
}

static http_resp *do_curl_req(const char *url, const char *post_body,
                              const char *bearer, bool is_form) {
    pthread_mutex_lock(&g_curl_mu);
    CURL *c = curl_get_handle();
    if (!c) {
        pthread_mutex_unlock(&g_curl_mu);
        http_resp *r = (http_resp *)calloc(1, sizeof(http_resp));
        r->status = -1;
        return r;
    }

    struct mem_buf mb = {0};
    mb_init(&mb, 65536);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 0L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: streamdeck-twitch/1.0");

    if (g_extra_header[0])
        headers = curl_slist_append(headers, g_extra_header);

    if (bearer && bearer[0]) {
        char auth[2048];
        int n = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        if (n > 0 && (size_t)n < sizeof(auth))
            headers = curl_slist_append(headers, auth);
    }

    if (post_body) {
        headers = curl_slist_append(headers,
            is_form ? "Content-Type: application/x-www-form-urlencoded"
                    : "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }

    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

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
    pthread_mutex_unlock(&g_curl_mu);
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
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    curl_global_cleanup();
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