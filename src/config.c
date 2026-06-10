#include "config.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

config g_config;

/* ---------- 内部ヘルパー ---------- */

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

static int build_path(char *buf, size_t sz, const char *suffix)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    int n = snprintf(buf, sz, "%s%s", home, suffix);
    return (n < 0 || (size_t)n >= sz) ? -1 : 0;
}

static int get_dir(char *buf, size_t sz)
{
    return build_path(buf, sz, CONFIG_DIR);
}

static int get_full_path(char *buf, size_t sz)
{
    char dir[1024];
    if (get_dir(dir, sizeof(dir)) < 0) return -1;
    int n = snprintf(buf, sz, "%s%s", dir, CONFIG_FILE);
    return (n < 0 || (size_t)n >= sz) ? -1 : 0;
}

/* ---------- API ---------- */

void config_defaults(config *cfg)
{
    memset(cfg, 0, sizeof(config));
}

int config_load(config *cfg)
{
    char path[1024];
    if (get_full_path(path, sizeof(path)) < 0) {
        errorLog("config path too long");
        return -1;
    }

    /* binary モードで開く: ftell/fread の整合性確保 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        infoLog("config file not found: %s", path);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *data = malloc((size_t)len + 1);
    if (!data) { fclose(f); return -1; }

    size_t rd = fread(data, 1, (size_t)len, f);
    fclose(f);
    if ((long)rd != len) {
        free(data);
        return -1;
    }
    data[len] = '\0';

    json_val *j = json_parse(data);
    free(data);
    if (!j || j->type != JSON_OBJ) {
        json_free(j);
        return -1;
    }

    json_val *v;
    v = json_obj_get(j, "client_id");      json_strcpy(cfg->client_id,      sizeof(cfg->client_id),      v);
    v = json_obj_get(j, "client_secret");  json_strcpy(cfg->client_secret,  sizeof(cfg->client_secret),  v);
    v = json_obj_get(j, "access_token");   json_strcpy(cfg->access_token,   sizeof(cfg->access_token),   v);
    v = json_obj_get(j, "refresh_token");  json_strcpy(cfg->refresh_token,  sizeof(cfg->refresh_token),  v);
    v = json_obj_get(j, "user_id");        json_strcpy(cfg->user_id,        sizeof(cfg->user_id),        v);
    v = json_obj_get(j, "channel_name");   json_strcpy(cfg->channel_name,   sizeof(cfg->channel_name),   v);

    json_free(j);
    infoLog("config loaded: %s", path);
    return 0;
}

int config_save(config *cfg)
{
    char dir[1024], path[1024];
    if (get_dir(dir, sizeof(dir)) < 0 || get_full_path(path, sizeof(path)) < 0) {
        errorLog("config path too long");
        return -1;
    }

    /* stat + mkdir の2回システムコールを mkdir1回に統合 */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        errorLog("mkdir failed: %s", dir);
        return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        errorLog("fopen failed: %s", path);
        return -1;
    }

    int n = fprintf(f,
        "{\n"
        "  \"client_id\": \"%s\",\n"
        "  \"client_secret\": \"%s\",\n"
        "  \"access_token\": \"%s\",\n"
        "  \"refresh_token\": \"%s\",\n"
        "  \"user_id\": \"%s\",\n"
        "  \"channel_name\": \"%s\"\n"
        "}\n",
        cfg->client_id, cfg->client_secret,
        cfg->access_token, cfg->refresh_token,
        cfg->user_id, cfg->channel_name);

    fclose(f);

    if (n < 0) {
        errorLog("fprintf failed: %s", path);
        return -1;
    }

    infoLog("config saved: %s", path);
    return 0;
}