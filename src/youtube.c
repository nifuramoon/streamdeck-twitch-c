#include "youtube.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "config.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

#define YT_API_BASE "https://www.googleapis.com/youtube/v3"
#define YT_CACHE_FILE "yt_subs.json"

static inline void json_strcpy(char *dst, size_t size, const json_val *v)
{
    if (v && v->type == JSON_STR) {
        size_t len = strlen(v->str);
        if (len >= size) len = size - 1;
        memcpy(dst, v->str, len);
        dst[len] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static inline int json_get_int(const json_val *v, int default_val)
{
    return (v && v->type == JSON_NUM) ? (int)v->num : default_val;
}

static void cache_path(char *buf, size_t sz, const char *name)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sz, "%s/.cache/streamdeck-twitch/%s", home, name);
}

static int api_get(const char *url, json_val **out)
{
    http_resp *r = http_get(url, NULL);
    if (!r || r->status != 200) {
        errorLog("YT API: %s (status %d)", url, r ? r->status : -1);
        http_resp_free(r);
        return -1;
    }
    *out = json_parse(r->body);
    http_resp_free(r);
    if (!*out) return -1;
    return 0;
}

/* subscriptions: OAuth 必須、購読一覧を取得してキャッシュに保存 */
int yt_fetch_subs(const char *bearer, char *channel_ids[], char *names[],
                  int max, int *out_len)
{
    char url[4096];
    snprintf(url, sizeof(url),
             "%s/subscriptions?part=snippet&mine=true&maxResults=50",
             YT_API_BASE);

    http_resp *r = http_get(url, bearer);
    if (!r || r->status != 200) {
        errorLog("YT subs fetch failed (status %d)", r ? r->status : -1);
        http_resp_free(r);
        return -1;
    }

    json_val *j = json_parse(r->body);
    http_resp_free(r);
    if (!j) return -1;

    json_val *items = json_obj_get(j, "items");
    if (!items || items->type != JSON_ARR) { json_free(j); return -1; }

    int n = 0;
    for (size_t i = 0; i < items->arr.len && n < max; i++) {
        json_val *item = json_arr_get(items, i);
        json_val *snippet = json_obj_get(item, "snippet");
        if (!snippet) continue;

        json_val *res = json_obj_get(snippet, "resourceId");
        if (!res) continue;
        json_val *cid = json_obj_get(res, "channelId");
        if (!cid || cid->type != JSON_STR) continue;

        channel_ids[n] = strdup(cid->str);

        json_val *title = json_obj_get(snippet, "title");
        names[n] = (title && title->type == JSON_STR) ? strdup(title->str) : strdup("");

        n++;
    }

    json_free(j);

    char path[1024];
    cache_path(path, sizeof(path), YT_CACHE_FILE);
    FILE *f = fopen(path, "w");
    if (f) {
        fputc('[', f);
        for (int i = 0; i < n; i++) {
            if (i > 0) fputc(',', f);
            fprintf(f, "{\"id\":\"%s\",\"name\":\"%s\"}", channel_ids[i], names[i]);
        }
        fputc(']', f);
        fclose(f);
    }

    *out_len = n;
    return 0;
}

int yt_load_subs_cache(char *channel_ids[], char *names[], int max, int *out_len)
{
    char path[1024];
    cache_path(path, sizeof(path), YT_CACHE_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return -1; }

    char *data = malloc((size_t)len + 1);
    fread(data, 1, (size_t)len, f);
    fclose(f);
    data[len] = '\0';

    json_val *j = json_parse(data);
    free(data);
    if (!j || j->type != JSON_ARR) { json_free(j); return -1; }

    int n = 0;
    for (size_t i = 0; i < j->arr.len && n < max; i++) {
        json_val *item = json_arr_get(j, i);
        json_val *idv = json_obj_get(item, "id");
        json_val *nv = json_obj_get(item, "name");
        if (idv && idv->type == JSON_STR) {
            channel_ids[n] = strdup(idv->str);
            names[n] = (nv && nv->type == JSON_STR) ? strdup(nv->str) : strdup("");
            n++;
        }
    }
    *out_len = n;
    json_free(j);
    return 0;
}

/* 一括 search でライブ配信を取得。各チャンネル1回のAPIコールに抑える */
int yt_get_trending_live(const char *api_key, char *channel_ids[], char *names[],
                         char titles[][256], char thumbs[][MAX_STR], int *viewers,
                         int max, int *out_len, int max_requests)
{
    char url[8192];
    snprintf(url, sizeof(url),
             "%s/search?part=snippet&eventType=live&type=video&regionCode=JP"
             "&maxResults=%d&key=%s",
             YT_API_BASE, max > 50 ? 50 : max, api_key);

    json_val *j = NULL;
    if (api_get(url, &j) < 0) return -1;
    *out_len = 0;

    json_val *items = json_obj_get(j, "items");
    if (!items || items->type != JSON_ARR) { json_free(j); return 0; }

    for (size_t i = 0; i < items->arr.len && (int)*out_len < max; i++) {
        json_val *item = json_arr_get(items, i);
        json_val *snippet = json_obj_get(item, "snippet");
        if (!snippet) continue;

        json_val *chid = json_obj_get(snippet, "channelId");
        if (!chid || chid->type != JSON_STR) continue;

        int idx = *out_len;
        channel_ids[idx] = strdup(chid->str);

        json_val *chtitle = json_obj_get(snippet, "channelTitle");
        names[idx] = (chtitle && chtitle->type == JSON_STR) ? strdup(chtitle->str) : strdup("");

        json_val *t = json_obj_get(snippet, "title");
        titles[idx][0] = '\0';
        if (t && t->type == JSON_STR) json_strcpy(titles[idx], 256, t);

        json_val *thumbs_o = json_obj_get(snippet, "thumbnails");
        thumbs[idx][0] = '\0';
        if (thumbs_o) {
            json_val *med = json_obj_get(thumbs_o, "medium");
            if (!med) med = json_obj_get(thumbs_o, "default");
            if (med) json_strcpy(thumbs[idx], MAX_STR, json_obj_get(med, "url"));
        }

        viewers[idx] = 0;
        (*out_len)++;
    }

    json_free(j);
    return 0;
}
