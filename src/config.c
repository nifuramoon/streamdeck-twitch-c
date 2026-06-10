#include "config.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

config g_config;

static void get_path(char *buf, size_t sz) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sz, "%s%s", home, CONFIG_DIR);
}

static void get_full_path(char *buf, size_t sz) {
    char dir[1024];
    get_path(dir, sizeof(dir));
    snprintf(buf, sz, "%s%s", dir, CONFIG_FILE);
}

void config_defaults(config *cfg) {
    memset(cfg, 0, sizeof(config));
}

int config_load(config *cfg) {
    char path[1024];
    get_full_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        infoLog("config file not found: %s", path);
        return -1;
    }

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
    if (!j || j->type != JSON_OBJ) {
        json_free(j);
        return -1;
    }

    json_val *v;
    v = json_obj_get(j, "client_id"); if (v && v->type == JSON_STR) strncpy(cfg->client_id, v->str, MAX_STR-1);
    v = json_obj_get(j, "client_secret"); if (v && v->type == JSON_STR) strncpy(cfg->client_secret, v->str, MAX_STR-1);
    v = json_obj_get(j, "access_token"); if (v && v->type == JSON_STR) strncpy(cfg->access_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "refresh_token"); if (v && v->type == JSON_STR) strncpy(cfg->refresh_token, v->str, MAX_STR-1);
    v = json_obj_get(j, "user_id"); if (v && v->type == JSON_STR) strncpy(cfg->user_id, v->str, MAX_STR-1);
    v = json_obj_get(j, "channel_name"); if (v && v->type == JSON_STR) strncpy(cfg->channel_name, v->str, MAX_STR-1);

    json_free(j);
    infoLog("config loaded: %s", path);
    return 0;
}

int config_save(config *cfg) {
    char dir[1024], path[1024];
    get_path(dir, sizeof(dir));
    get_full_path(path, sizeof(path));

    struct stat st = {0};
    if (stat(dir, &st) == -1)
        mkdir(dir, 0755);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
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

    infoLog("config saved: %s", path);
    return 0;
}
