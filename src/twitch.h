#ifndef TWITCH_H
#define TWITCH_H

#include "common.h"

typedef struct {
    char id[MAX_STR];
    char login[MAX_STR];
    char display_name[MAX_STR];
    char description[MAX_STR];
    int  viewer_count;      /* 互換性維持: stream情報混在 */
    bool is_live;
    char game_name[MAX_STR];
    char title[MAX_STR];
    char profile_pic[MAX_STR];
} twitch_user;

typedef struct {
    twitch_user *items;
    size_t len;
} twitch_follow_list;

typedef struct {
    char id[MAX_STR];
    char title[MAX_STR];
    char user_name[MAX_STR];
    int  viewer_count;
    char game_name[MAX_STR];
    bool is_live;
} twitch_stream;

int  twitch_get_user(const char *login, twitch_user *u);
int  twitch_get_stream(const char *user_id, twitch_stream *s);
int  twitch_get_follows(const char *user_id, twitch_follow_list *fl);
void twitch_free_follows(twitch_follow_list *fl);

#endif