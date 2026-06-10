#ifndef HTTP_H
#define HTTP_H

#include "common.h"

typedef struct {
    int status;
    char *body;
    size_t body_len;
} http_resp;

http_resp *http_get(const char *url, const char *bearer_token);
http_resp *http_post(const char *url, const char *body, const char *bearer_token);
http_resp *http_post_form(const char *url, const char *body, const char *bearer_token);
void http_resp_free(http_resp *r);
void http_close_all(void);
void http_set_extra_header(const char *hdr);

#endif