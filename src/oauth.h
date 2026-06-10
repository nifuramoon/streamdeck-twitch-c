#ifndef OAUTH_H
#define OAUTH_H

#include "common.h"
#include "token.h"

int oauth_start_server(void);
int oauth_open_browser(const char *client_id);
int oauth_wait_for_result(int timeout_sec);
int oauth_exchange_code(const char *client_id, const char *client_secret,
                        const char *code, token_info *t);
void oauth_stop_server(void);
const char *oauth_get_error(void);
const char *oauth_get_code(void);
int oauth_is_server_running(void);

#endif
