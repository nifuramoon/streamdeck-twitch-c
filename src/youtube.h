#ifndef YOUTUBE_H
#define YOUTUBE_H

#include "common.h"

int yt_fetch_subs(const char *bearer, char *channel_ids[], char *names[],
                  int max, int *out_len);
int yt_load_subs_cache(char *channel_ids[], char *names[], int max, int *out_len);
int yt_get_trending_live(const char *api_key, char *channel_ids[], char *names[],
                         char titles[][256], char thumbs[][MAX_STR],
                         char video_ids[][MAX_STR], int *viewers,
                         int max, int *out_len);
int yt_batch_viewers(const char *api_key, char video_ids[][MAX_STR], int count,
                     int *viewers_out);

#endif
