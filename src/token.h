#ifndef TOKEN_H
#define TOKEN_H

#include "common.h"
#include <time.h>

typedef struct {
    char access_token[MAX_STR];
    char refresh_token[MAX_STR];
    char user_id[MAX_STR];
    char login_name[MAX_STR];
    char display_name[MAX_STR];
    char client_id[MAX_STR];
    char scope[1024];
    time_t expires_at;
    time_t created_at;
    time_t last_used;
} token_info;

int  token_load(token_info *t);
int  token_save(const token_info *t);
int  token_validate(const token_info *t);
void token_refresh(const char *client_id, const char *client_secret,
                   token_info *t);

#endif
