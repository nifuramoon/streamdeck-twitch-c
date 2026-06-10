#include "twitch.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define API_BASE "https://api.twitch.tv/helix"

/* ---------- 内部ヘルパー ---------- */

/* スタック上にURLを組み立て、strdup/Freeのオーバーヘッドを排除 */
static int build_url(char *buf, size_t size,
                     const char *endpoint,
                     const char *param, const char *val)
{
    int n = snprintf(buf, size, "%s/%s?%s=%s",
                     API_BASE, endpoint, param, val);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static int api_call(const char *url, json_val **out)
{
    http_resp *r = http_get(url, g_config.access_token);
    if (!r || r->status != 200) {
        errorLog("API call failed: %s (status %d)",
                 url, r ? r->status : -1);
        http_resp_free(r);
        return -1;
    }

    *out = json_parse(r->body);
    http_resp_free(r);
    if (!*out) {
        errorLog("JSON parse failed: %s", url);
        return -1;
    }
    return 0;
}

/* JSON文字列を安全にコピー。必ずnull終端し、余計な0埋めをしない */
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

static inline json_val* get_data_array(json_val *j)
{
    json_val *data = json_obj_get(j, "data");
    return (data && data->type == JSON_ARR) ? data : NULL;
}

/* ---------- API実装 ---------- */

int twitch_get_user(const char *login, twitch_user *u)
{
    char url[4096];
    if (build_url(url, sizeof(url), "users", "login", login) < 0)
        return -1;

    json_val *j = NULL;
    if (api_call(url, &j) < 0)
        return -1;

    json_val *data = get_data_array(j);
    if (!data || data->arr.len == 0) {
        json_free(j);
        return -1;
    }

    const json_val *user = json_arr_get(data, 0);
    memset(u, 0, sizeof(*u));   /* ゼロクリアで未初期化防止 */

    json_strcpy(u->id,           sizeof(u->id),           json_obj_get(user, "id"));
    json_strcpy(u->login,        sizeof(u->login),        json_obj_get(user, "login"));
    json_strcpy(u->display_name, sizeof(u->display_name), json_obj_get(user, "display_name"));
    json_strcpy(u->description,  sizeof(u->description),  json_obj_get(user, "description"));
    json_strcpy(u->profile_pic,  sizeof(u->profile_pic),  json_obj_get(user, "profile_image_url"));

    json_free(j);
    return 0;
}

int twitch_get_stream(const char *user_id, twitch_stream *s)
{
    char url[4096];
    if (build_url(url, sizeof(url), "streams", "user_id", user_id) < 0)
        return -1;

    json_val *j = NULL;
    if (api_call(url, &j) < 0)
        return -1;

    json_val *data = get_data_array(j);
    memset(s, 0, sizeof(*s));

    if (data && data->arr.len > 0) {
        const json_val *stream = json_arr_get(data, 0);
        json_strcpy(s->id,        sizeof(s->id),        json_obj_get(stream, "id"));
        json_strcpy(s->title,     sizeof(s->title),     json_obj_get(stream, "title"));
        json_strcpy(s->user_name, sizeof(s->user_name), json_obj_get(stream, "user_name"));
        json_strcpy(s->game_name, sizeof(s->game_name), json_obj_get(stream, "game_name"));
        s->viewer_count = json_get_int(json_obj_get(stream, "viewer_count"), 0);
        s->is_live = true;
    } else {
        s->is_live = false;
    }

    json_free(j);
    return 0;
}

int twitch_get_follows(const char *user_id, twitch_follow_list *fl)
{
    char url[4096];
    int n = snprintf(url, sizeof(url),
                     "%s/channels/followed?user_id=%s&first=100",
                     API_BASE, user_id);
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    json_val *j = NULL;
    if (api_call(url, &j) < 0)
        return -1;

    json_val *data = get_data_array(j);
    if (!data) {
        json_free(j);
        return -1;
    }

    fl->len = data->arr.len;
    fl->items = calloc(fl->len, sizeof(twitch_user));
    if (!fl->items) {
        errorLog("calloc failed in twitch_get_follows");
        json_free(j);
        return -1;
    }

    for (size_t i = 0; i < fl->len; i++) {
        const json_val *br = json_arr_get(data, i);
        twitch_user *u = &fl->items[i];
        memset(u, 0, sizeof(*u));

        json_strcpy(u->id,           sizeof(u->id),           json_obj_get(br, "broadcaster_id"));
        json_strcpy(u->login,        sizeof(u->login),        json_obj_get(br, "broadcaster_login"));
        json_strcpy(u->display_name, sizeof(u->display_name), json_obj_get(br, "broadcaster_name"));
    }

    json_free(j);
    return 0;
}

void twitch_free_follows(twitch_follow_list *fl)
{
    if (fl && fl->items) {
        free(fl->items);
        fl->items = NULL;
        fl->len = 0;
    }
}