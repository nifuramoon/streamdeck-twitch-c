#include "oauth.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>

static int g_oauth_fd = -1;
static pthread_t g_oauth_tid;
static volatile int g_oauth_done = 0;
static int g_oauth_ok = 0;
static char g_oauth_code[MAX_STR] = {0};
static char g_oauth_error[1024] = {0};

static const char *oauth_success_html =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>OK</title>"
    "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#1a1a2e;color:#eee}"
    "h1{color:#4ecdc4}</style></head>"
    "<body><h1>\xe8\xaa\x8d\xe8\xa8\xbc\xe6\x88\x90\xe5\x8a\x9f</h1>"
    "<p>StreamDeck\xe3\x81\x8c\xe3\x83\x88\xe3\x83\xbc\xe3\x82\xaf\xe3\x83\xb3\xe3\x82\x92"
    "\xe5\x8f\x96\xe5\xbe\x97\xe4\xb8\xad...</p>"
    "<p style=\"color:#aaa\">\xe3\x81\x93\xe3\x81\xae\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3"
    "\xe3\x83\x89\xe3\x82\xa6\xe3\x81\xaf\xe9\x96\x89\xe3\x81\x98\xe3\x81\xa6\xe3\x81\x8f"
    "\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84</p></body></html>";

static const char *oauth_fail_html_fmt =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>Fail</title>"
    "<style>body{font-family:sans-serif;text-align:center;padding:50px;background:#1a1a2e;color:#eee}"
    "h1{color:#ff6b6b}</style></head>"
    "<body><h1>\xe8\xaa\x8d\xe8\xa8\xbc\xe5\xa4\xb1\xe6\x95\x97</h1>"
    "<p>%s</p>"
    "<p style=\"color:#aaa\">\xe3\x81\x93\xe3\x81\xae\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3"
    "\xe3\x83\x89\xe3\x82\xa6\xe3\x81\xaf\xe9\x96\x89\xe3\x81\x98\xe3\x81\xa6\xe3\x81\x8f"
    "\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84</p></body></html>";

static void handle_client(int client_fd) {
    char buf[4096];
    int n = (int)read(client_fd, buf, sizeof(buf)-1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    /* Parse request line */
    char method[16], path[4096];
    if (sscanf(buf, "%15s %4095s", method, path) < 2) {
        close(client_fd);
        return;
    }

    /* Extract query params */
    char *code_val = NULL;
    char *err_val = NULL;
    char *desc_val = NULL;

    char *q = strchr(path, '?');
    if (q) {
        q++;
        char *save;
        char *tok = strtok_r(q, "&", &save);
        while (tok) {
            if (strncmp(tok, "code=", 5) == 0) code_val = tok + 5;
            else if (strncmp(tok, "error=", 6) == 0) err_val = tok + 6;
            else if (strncmp(tok, "error_description=", 18) == 0) {
                desc_val = tok + 18;
                /* URL decode in-place */
                char *src = desc_val, *dst = desc_val;
                while (*src) {
                    if (*src == '+' ) { *dst++ = ' '; src++; }
                    else if (*src == '%' && src[1] && src[2]) {
                        unsigned int c;
                        sscanf(src+1, "%2x", &c);
                        *dst++ = (char)c;
                        src += 3;
                    } else *dst++ = *src++;
                }
                *dst = '\0';
            }
            tok = strtok_r(NULL, "&", &save);
        }
    }

    if (err_val) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Authorization denied: %s%s%s",
                 err_val, desc_val ? " - " : "", desc_val ? desc_val : "");
        char resp[4096];
        snprintf(resp, sizeof(resp), oauth_fail_html_fmt, g_oauth_error);
        write(client_fd, resp, strlen(resp));
        close(client_fd);
        g_oauth_ok = 0;
        g_oauth_done = 1;
        return;
    }

    if (code_val) {
        /* URL decode code */
        char *src = code_val, *dst = g_oauth_code;
        while (*src && (size_t)(dst - g_oauth_code) < MAX_STR - 1) {
            if (*src == '%' && src[1] && src[2]) {
                unsigned int c;
                sscanf(src+1, "%2x", &c);
                *dst++ = (char)c;
                src += 3;
            } else *dst++ = *src++;
        }
        *dst = '\0';
    }

    write(client_fd, oauth_success_html, strlen(oauth_success_html));
    close(client_fd);
    g_oauth_ok = 1;
    g_oauth_done = 1;
}

static void *oauth_server_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Port 8080 already in use");
        return NULL;
    }
    listen(fd, 5);
    g_oauth_fd = fd;
    infoLog("OAuth server listening on :8080");

    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (!g_oauth_done) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            handle_client(client_fd);
        }
    }

    close(fd);
    g_oauth_fd = -1;
    return NULL;
}

int oauth_start_server(void) {
    g_oauth_done = 0;
    g_oauth_ok = 0;
    g_oauth_code[0] = '\0';
    g_oauth_error[0] = '\0';

    if (pthread_create(&g_oauth_tid, NULL, oauth_server_thread, NULL) != 0) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Failed to start server thread");
        return -1;
    }
    pthread_detach(g_oauth_tid);
    return 0;
}

int oauth_open_browser(const char *client_id) {
    char url[4096];
    const char *scope = "user:read:email user:read:follows user:read:broadcast user:write:chat chat:read";
    snprintf(url, sizeof(url),
        "https://id.twitch.tv/oauth2/authorize"
        "?client_id=%s"
        "&redirect_uri=http://localhost:8080"
        "&response_type=code"
        "&scope=%s",
        client_id, scope);

    char cmd[4120];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' &", url);
    int r = system(cmd);
    if (r != 0) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Failed to open browser (xdg-open)");
        return -1;
    }
    infoLog("OAuth URL opened in browser");
    return 0;
}

int oauth_wait_for_result(int timeout_sec) {
    for (int i = 0; i < timeout_sec * 10; i++) {
        if (g_oauth_done) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    if (!g_oauth_done) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Timeout waiting for authorization (%d seconds)", timeout_sec);
        return -1;
    }
    if (!g_oauth_ok) return -1;
    return 0;
}

int oauth_exchange_code(const char *client_id, const char *client_secret,
                        const char *code, token_info *t) {
    if (!code || !*code) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "No authorization code");
        return -1;
    }

    char body[4096];
    snprintf(body, sizeof(body),
        "client_id=%s"
        "&client_secret=%s"
        "&code=%s"
        "&grant_type=authorization_code"
        "&redirect_uri=http://localhost:8080",
        client_id, client_secret, code);

    http_resp *r = http_post_form("https://id.twitch.tv/oauth2/token", body, NULL);
    if (!r) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Network error: could not reach Twitch");
        return -1;
    }

    if (r->status != 200) {
        if (r->body && strstr(r->body, "invalid client secret")) {
            snprintf(g_oauth_error, sizeof(g_oauth_error),
                     "Client Secretが無効です。config.jsonを確認してください");
        } else if (r->body && strstr(r->body, "invalid")) {
            snprintf(g_oauth_error, sizeof(g_oauth_error),
                     "認証情報無効: %s", r->body);
        } else {
            snprintf(g_oauth_error, sizeof(g_oauth_error),
                     "Token exchange failed (HTTP %d): %s",
                     r->status, r->body ? r->body : "(empty)");
        }
        http_resp_free(r);
        return -1;
    }

    json_val *j = json_parse(r->body);
    http_resp_free(r);
    if (!j) {
        snprintf(g_oauth_error, sizeof(g_oauth_error),
                 "Failed to parse token response");
        return -1;
    }

    memset(t, 0, sizeof(*t));
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
    t->created_at = time(NULL);
    t->last_used = time(NULL);
    strncpy(t->client_id, client_id, MAX_STR-1);
    strncpy(t->scope, "user:read:email user:read:follows user:read:broadcast user:write:chat chat:read",
            sizeof(t->scope)-1);
    strncpy(t->login_name, "unknown", MAX_STR-1);
    strncpy(t->display_name, "unknown", MAX_STR-1);

    json_free(j);
    infoLog("OAuth token exchange successful");

    token_save(t);
    return 0;
}

void oauth_stop_server(void) {
    g_oauth_done = 1;
    if (g_oauth_fd >= 0) {
        close(g_oauth_fd);
        g_oauth_fd = -1;
    }
}

const char *oauth_get_error(void) {
    return g_oauth_error;
}

const char *oauth_get_code(void) {
    return g_oauth_code;
}

int oauth_is_server_running(void) {
    return g_oauth_fd >= 0;
}
