#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

typedef struct {
    char client_id[MAX_STR];
    char client_secret[MAX_STR];
    char access_token[MAX_STR];
    char refresh_token[MAX_STR];
    char user_id[MAX_STR];
    char channel_name[MAX_STR];
    char youtube_api_key[MAX_STR];
    char yt_client_id[MAX_STR];
    char yt_client_secret[MAX_STR];
    char yt_access_token[MAX_STR];
    char yt_refresh_token[MAX_STR];
} config;

int  config_load(config *cfg);
int  config_save(config *cfg);
void config_defaults(config *cfg);
int  config_set_yt_oauth(const char *client_id, const char *client_secret);
int  config_set_yt_token(const char *access_token, const char *refresh_token);

extern config g_config;

#endif
