#include "token.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static const char *get_dir(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    static char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.config/streamdeck-twitch", home);
    mkdir(dir, 0755);
    char backups[1100];
    snprintf(backups, sizeof(backups), "%s/backups", dir);
    mkdir(backups, 0755);
    return dir;
}

static void token_path(char *out, size_t sz) {
    snprintf(out, sz, "%s/tokens.json", get_dir());
}

static int token_to_json(const token_info *t, char *out, size_t sz) {
    char expires_buf[64] = {0}, created_buf[64] = {0}, last_buf[64] = {0};
    if (t->expires_at) {
        struct tm *tm = localtime(&t->expires_at);
        if (tm) strftime(expires_buf, sizeof(expires_buf), "%Y-%m-%dT%H:%M:%S", tm);
    }
    if (t->created_at) {
        struct tm *tm = localtime(&t->created_at);
        if (tm) strftime(created_buf, sizeof(created_buf), "%Y-%m-%dT%H:%M:%S", tm);
    }
    if (t->last_used) {
        struct tm *tm = localtime(&t->last_used);
        if (tm) strftime(last_buf, sizeof(last_buf), "%Y-%m-%dT%H:%M:%S", tm);
    }

    return snprintf(out, sz,
        "{\n"
        "  \"access_token\": \"%s\",\n"
        "  \"refresh_token\": \"%s\",\n"
        "  \"user_id\": \"%s\",\n"
        "  \"login_name\": \"%s\",\n"
        "  \"display_name\": \"%s\",\n"
        "  \"client_id\": \"%s\",\n"
        "  \"scope\": \"%s\",\n"
        "  \"expires_at\": \"%s\",\n"
        "  \"created_at\": \"%s\",\n"
        "  \"last_used\": \"%s\"\n"
        "}\n",
        t->access_token, t->refresh_token, t->user_id,
        t->login_name, t->display_name, t->client_id, t->scope,
        expires_buf, created_buf, last_buf);
}

static int parse_time(const char *s, time_t *out) {
    if (!s || !*s) { *out = 0; return 0; }
    struct tm tm = {0};
    if (strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) {
        *out = mktime(&tm);
        return 0;
    }
    *out = 0;
    return -1;
}

static int json_to_token(const char *json_str, token_info *t) {
    memset(t, 0, sizeof(*t));
    json_val *j = json_parse(json_str);
    if (!j || j->type != JSON_OBJ) { json_free(j); return -1; }

    json_val *v;
    v = json_obj_get(j, "access_token");
    if (v && v->type == JSON_STR) strncpy(t->access_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "refresh_token");
    if (v && v->type == JSON_STR) strncpy(t->refresh_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "user_id");
    if (v && v->type == JSON_STR) strncpy(t->user_id, v->str, MAX_STR-1);
    v = json_obj_get(j, "login_name");
    if (v && v->type == JSON_STR) strncpy(t->login_name, v->str, MAX_STR-1);
    v = json_obj_get(j, "display_name");
    if (v && v->type == JSON_STR) strncpy(t->display_name, v->str, MAX_STR-1);
    v = json_obj_get(j, "client_id");
    if (v && v->type == JSON_STR) strncpy(t->client_id, v->str, MAX_STR-1);
    v = json_obj_get(j, "scope");
    if (v && v->type == JSON_STR) strncpy(t->scope, v->str, sizeof(t->scope)-1);
    v = json_obj_get(j, "expires_at");
    if (v && v->type == JSON_STR) parse_time(v->str, &t->expires_at);
    v = json_obj_get(j, "created_at");
    if (v && v->type == JSON_STR) parse_time(v->str, &t->created_at);
    v = json_obj_get(j, "last_used");
    if (v && v->type == JSON_STR) parse_time(v->str, &t->last_used);

    json_free(j);
    return 0;
}

static void sync_last_backup(void) {
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/backups", get_dir());

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    char oldest[1100] = {0};
    time_t oldest_t = 0;
    int count = 0;
    while ((e = readdir(d))) {
        if (e->d_type == DT_REG || e->d_type == DT_UNKNOWN) {
            count++;
            struct stat st;
            char fp[1200];
            snprintf(fp, sizeof(fp), "%s/%s", dir, e->d_name);
            if (stat(fp, &st) == 0) {
                if (oldest_t == 0 || st.st_mtime < oldest_t) {
                    oldest_t = st.st_mtime;
                    strncpy(oldest, fp, sizeof(oldest)-1);
                }
            }
        }
    }
    closedir(d);

    if (count > 10 && oldest[0]) {
        remove(oldest);
        infoLog("Token backups >10, removed oldest: %s", oldest);
    }
}

int token_load(token_info *t) {
    memset(t, 0, sizeof(*t));
    char path[1024];
    token_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        warnLog("No token file found at %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    char *data = malloc((size_t)sz + 1);
    fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[sz] = '\0';

    int r = json_to_token(data, t);
    free(data);

    if (r == 0) {
        infoLog("Token loaded: %s (%s)", t->display_name, t->login_name);
    }
    return r;
}

int token_save(const token_info *t) {
    char path[1024];
    token_path(path, sizeof(path));

    char json[8192];
    token_to_json(t, json, sizeof(json));

    FILE *f = fopen(path, "w");
    if (!f) { errorLog("Failed to save token"); return -1; }
    fprintf(f, "%s", json);
    fclose(f);

    /* Backup */
    char backup[1200];
    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", tm);
    snprintf(backup, sizeof(backup), "%s/backups/tokens_%s_%s.json",
             get_dir(), t->login_name, timebuf);

    f = fopen(backup, "w");
    if (f) { fprintf(f, "%s", json); fclose(f); }

    sync_last_backup();
    infoLog("Token saved for %s", t->display_name[0] ? t->display_name : t->login_name);
    return 0;
}

int token_validate(const token_info *t) {
    if (!t->access_token[0]) return -1;
    if (t->expires_at > 0 && time(NULL) >= t->expires_at) {
        warnLog("Token expired");
        return -2;
    }
    if (t->created_at > 0 && (time(NULL) - t->created_at) > 60 * 24 * 3600) {
        warnLog("Token too old (>60 days)");
        return -3;
    }
    return 0;
}

void token_refresh(const char *client_id, const char *client_secret,
                   token_info *t) {
    if (!t->refresh_token[0]) {
        errorLog("No refresh token available");
        return;
    }

    char body[4096];
    snprintf(body, sizeof(body),
        "client_id=%s"
        "&client_secret=%s"
        "&grant_type=refresh_token"
        "&refresh_token=%s",
        client_id, client_secret, t->refresh_token);

    http_resp *r = http_post_form("https://id.twitch.tv/oauth2/token", body, NULL);
    if (!r || r->status != 200) {
        errorLog("Token refresh failed (status %d)", r ? r->status : -1);
        if (r) errorLog("body: %s", r->body ? r->body : "(empty)");
        http_resp_free(r);
        return;
    }

    json_val *j = json_parse(r->body);
    http_resp_free(r);
    if (!j) return;

    json_val *v;
    v = json_obj_get(j, "access_token");
    if (v && v->type == JSON_STR) strncpy(t->access_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "refresh_token");
    if (v && v->type == JSON_STR) strncpy(t->refresh_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "expires_in");
    if (v && v->type == JSON_NUM) {
        t->expires_at = time(NULL) + (time_t)v->num;
    } else {
        t->expires_at = time(NULL) + 24 * 3600;
    }
    t->last_used = time(NULL);

    json_free(j);
    token_save(t);
    infoLog("Token refreshed successfully");
}
