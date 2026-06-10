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
} config;

int  config_load(config *cfg);
int  config_save(config *cfg);
void config_defaults(config *cfg);

extern config g_config;

#endif
