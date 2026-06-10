#include "twitch.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define API_BASE "https://api.twitch.tv/helix"

static char *build_url(const char *endpoint, const char *param, const char *val) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/%s?%s=%s", API_BASE, endpoint, param, val);
    return strdup(buf);
}

static int api_call(const char *url, json_val **out) {
    http_resp *r = http_get(url, g_config.access_token);
    if (!r || r->status != 200) {
        errorLog("API call failed: %s (status %d)", url, r ? r->status : -1);
        http_resp_free(r);
        return -1;
    }
    *out = json_parse(r->body);
    http_resp_free(r);
    if (!*out) { errorLog("JSON parse failed"); return -1; }
    return 0;
}

int twitch_get_user(const char *login, twitch_user *u) {
    char *url = build_url("users", "login", login);
    json_val *j = NULL;
    if (api_call(url, &j) < 0) { free(url); return -1; }
    free(url);

    json_val *data = json_obj_get(j, "data");
    if (!data || data->type != JSON_ARR || data->arr.len == 0) {
        json_free(j); return -1;
    }
    json_val *user = json_arr_get(data, 0);

    json_val *v;
    v = json_obj_get(user, "id"); if (v && v->type == JSON_STR) strncpy(u->id, v->str, MAX_STR-1);
    v = json_obj_get(user, "login"); if (v && v->type == JSON_STR) strncpy(u->login, v->str, MAX_STR-1);
    v = json_obj_get(user, "display_name"); if (v && v->type == JSON_STR) strncpy(u->display_name, v->str, MAX_STR-1);
    v = json_obj_get(user, "description"); if (v && v->type == JSON_STR) strncpy(u->description, v->str, MAX_STR-1);
    v = json_obj_get(user, "profile_image_url"); if (v && v->type == JSON_STR) strncpy(u->profile_pic, v->str, MAX_STR-1);

    json_free(j);
    return 0;
}

int twitch_get_stream(const char *user_id, twitch_stream *s) {
    char *url = build_url("streams", "user_id", user_id);
    json_val *j = NULL;
    if (api_call(url, &j) < 0) { free(url); return -1; }
    free(url);

    json_val *data = json_obj_get(j, "data");
    s->is_live = false;
    if (data && data->type == JSON_ARR && data->arr.len > 0) {
        json_val *stream = json_arr_get(data, 0);
        json_val *v;
        v = json_obj_get(stream, "id"); if (v && v->type == JSON_STR) strncpy(s->id, v->str, MAX_STR-1);
        v = json_obj_get(stream, "title"); if (v && v->type == JSON_STR) strncpy(s->title, v->str, MAX_STR-1);
        v = json_obj_get(stream, "user_name"); if (v && v->type == JSON_STR) strncpy(s->user_name, v->str, MAX_STR-1);
        v = json_obj_get(stream, "game_name"); if (v && v->type == JSON_STR) strncpy(s->game_name, v->str, MAX_STR-1);
        v = json_obj_get(stream, "viewer_count"); if (v && v->type == JSON_NUM) s->viewer_count = (int)v->num;
        s->is_live = true;
    }

    json_free(j);
    return 0;
}

int twitch_get_follows(const char *user_id, twitch_follow_list *fl) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/channels/followed?user_id=%s&first=100",
             API_BASE, user_id);

    json_val *j = NULL;
    if (api_call(buf, &j) < 0) return -1;

    json_val *data = json_obj_get(j, "data");
    if (!data || data->type != JSON_ARR) { json_free(j); return -1; }

    fl->len = data->arr.len;
    fl->items = calloc(fl->len, sizeof(twitch_user));
    for (size_t i = 0; i < fl->len; i++) {
        json_val *br = json_arr_get(data, i);
        json_val *v;
        v = json_obj_get(br, "broadcaster_id"); if (v && v->type == JSON_STR) strncpy(fl->items[i].id, v->str, MAX_STR-1);
        v = json_obj_get(br, "broadcaster_login"); if (v && v->type == JSON_STR) strncpy(fl->items[i].login, v->str, MAX_STR-1);
        v = json_obj_get(br, "broadcaster_name"); if (v && v->type == JSON_STR) strncpy(fl->items[i].display_name, v->str, MAX_STR-1);
    }

    json_free(j);
    return 0;
}

void twitch_free_follows(twitch_follow_list *fl) {
    free(fl->items);
    fl->items = NULL;
    fl->len = 0;
}
