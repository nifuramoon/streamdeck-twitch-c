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
#include <spawn.h>
#include <errno.h>

extern char **environ;

static int g_oauth_fd = -1;
static pthread_t g_oauth_tid;
static volatile int g_oauth_done = 0;
static int g_oauth_ok = 0;
static char g_oauth_code[MAX_STR] = {0};
static char g_oauth_error[1024] = {0};

/* ---------- 内部ヘルパー ---------- */

static inline void safe_strcpy(char *dst, size_t size, const char *src) {
    if (!src || size == 0) return;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static inline int hex2byte(const char *s) {
    int h = hex_digit(s[0]);
    int l = hex_digit(s[1]);
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

static void url_decode_in_place(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%') {
            int c = hex2byte(src + 1);
            if (c >= 0) {
                *dst++ = (char)c;
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* HTML レスポンス（配列にしてコンパイル時長さ取得） */
static const char oauth_success_html[] =
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

static const char oauth_fail_html_fmt[] =
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

/* ---------- HTTP ハンドラ ---------- */

static void handle_client(int client_fd) {
    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    char method[16], path[4096];
    if (sscanf(buf, "%15s %4095s", method, path) < 2) {
        close(client_fd);
        return;
    }

    char *code_val = NULL;
    char *err_val = NULL;
    char *desc_val = NULL;

    char *q = strchr(path, '?');
    if (q) {
        *q++ = '\0';
        char *save;
        char *tok = strtok_r(q, "&", &save);
        while (tok) {
            if (strncmp(tok, "code=", 5) == 0) {
                code_val = tok + 5;
            } else if (strncmp(tok, "error=", 6) == 0) {
                err_val = tok + 6;
            } else if (strncmp(tok, "error_description=", 18) == 0) {
                desc_val = tok + 18;
                url_decode_in_place(desc_val);
            }
            tok = strtok_r(NULL, "&", &save);
        }
    }

    if (err_val) {
        int m = snprintf(g_oauth_error, sizeof(g_oauth_error),
                         "Authorization denied: %s%s%s",
                         err_val,
                         desc_val ? " - " : "",
                         desc_val ? desc_val : "");
        if (m > 0) {
            char resp[4096];
            int r = snprintf(resp, sizeof(resp), oauth_fail_html_fmt, g_oauth_error);
            if (r > 0) (void)write(client_fd, resp, (size_t)r);
        }
        close(client_fd);
        g_oauth_ok = 0;
        g_oauth_done = 1;
        return;
    }

    if (code_val) {
        char *src = code_val, *dst = g_oauth_code;
        size_t left = MAX_STR - 1;
        while (*src && left) {
            if (*src == '%') {
                int c = hex2byte(src + 1);
                if (c >= 0) {
                    *dst++ = (char)c;
                    src += 3;
                    left--;
                    continue;
                }
            }
            *dst++ = *src++;
            left--;
        }
        *dst = '\0';
    }

    (void)write(client_fd, oauth_success_html, sizeof(oauth_success_html) - 1);
    close(client_fd);
    g_oauth_ok = 1;
    g_oauth_done = 1;
}

/* ---------- サーバスレッド ---------- */

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
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Port 8080 already in use");
        return NULL;
    }

    if (listen(fd, 5) < 0) {
        close(fd);
        return NULL;
    }

    g_oauth_fd = fd;
    infoLog("OAuth server listening on :8080");

    while (!g_oauth_done) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                continue;
            break;
        }
        handle_client(client_fd);
    }

    close(fd);
    g_oauth_fd = -1;
    return NULL;
}

/* ---------- 公開API ---------- */

int oauth_start_server(void) {
    g_oauth_done = 0;
    g_oauth_ok = 0;
    g_oauth_code[0] = '\0';
    g_oauth_error[0] = '\0';

    if (pthread_create(&g_oauth_tid, NULL, oauth_server_thread, NULL) != 0) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Failed to start server thread");
        return -1;
    }
    pthread_detach(g_oauth_tid);
    return 0;
}

int oauth_open_browser(const char *client_id) {
    static const char scope[] =
        "user:read:email user:read:follows user:read:broadcast user:write:chat chat:read";

    char url[4096];
    int n = snprintf(url, sizeof(url),
        "https://id.twitch.tv/oauth2/authorize"
        "?client_id=%s"
        "&redirect_uri=http://localhost:8080"
        "&response_type=code"
        "&scope=%s",
        client_id, scope);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "OAuth URL too long");
        return -1;
    }

    /* system() 代替: 軽量な posix_spawnp */
    pid_t pid;
    char *argv[] = {"xdg-open", url, NULL};
    if (posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ) != 0) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Failed to open browser (xdg-open)");
        return -1;
    }
    infoLog("OAuth URL opened in browser");
    return 0;
}

int oauth_wait_for_result(int timeout_sec) {
    for (int i = 0; i < timeout_sec * 10; i++) {
        if (g_oauth_done) break;
        usleep(100000); /* 100ms */
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
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "No authorization code");
        return -1;
    }

    char body[4096];
    int n = snprintf(body, sizeof(body),
        "client_id=%s"
        "&client_secret=%s"
        "&code=%s"
        "&grant_type=authorization_code"
        "&redirect_uri=http://localhost:8080",
        client_id, client_secret, code);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Request body too long");
        return -1;
    }

    http_resp *r = http_post_form("https://id.twitch.tv/oauth2/token", body, NULL);
    if (!r) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Network error: could not reach Twitch");
        return -1;
    }

    if (r->status != 200) {
        if (r->body && strstr(r->body, "invalid client secret")) {
            safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                        "Client Secretが無効です。config.jsonを確認してください");
        } else if (r->body && strstr(r->body, "invalid")) {
            int m = snprintf(g_oauth_error, sizeof(g_oauth_error),
                             "認証情報無効: %s", r->body);
            if (m < 0 || (size_t)m >= sizeof(g_oauth_error))
                safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                            "認証情報無効");
        } else {
            int m = snprintf(g_oauth_error, sizeof(g_oauth_error),
                             "Token exchange failed (HTTP %d): %s",
                             r->status, r->body ? r->body : "(empty)");
            if (m < 0 || (size_t)m >= sizeof(g_oauth_error))
                safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                            "Token exchange failed");
        }
        http_resp_free(r);
        return -1;
    }

    json_val *j = json_parse(r->body);
    http_resp_free(r);
    if (!j) {
        safe_strcpy(g_oauth_error, sizeof(g_oauth_error),
                    "Failed to parse token response");
        return -1;
    }

    memset(t, 0, sizeof(*t));
    json_val *v;

    v = json_obj_get(j, "access_token");
    if (v && v->type == JSON_STR) safe_strcpy(t->access_token, sizeof(t->access_token), v->str);
    v = json_obj_get(j, "refresh_token");
    if (v && v->type == JSON_STR) safe_strcpy(t->refresh_token, sizeof(t->refresh_token), v->str);

    time_t now = time(NULL);
    v = json_obj_get(j, "expires_in");
    if (v && v->type == JSON_NUM)
        t->expires_at = now + (time_t)v->num;
    else
        t->expires_at = now + 24 * 3600;

    t->created_at = now;
    t->last_used = now;
    safe_strcpy(t->client_id, sizeof(t->client_id), client_id);

    static const char default_scope[] =
        "user:read:email user:read:follows user:read:broadcast user:write:chat chat:read";
    safe_strcpy(t->scope, sizeof(t->scope), default_scope);

    safe_strcpy(t->login_name, sizeof(t->login_name), "unknown");
    safe_strcpy(t->display_name, sizeof(t->display_name), "unknown");

    json_free(j);
    infoLog("OAuth token exchange successful");
    token_save(t);
    return 0;
}

void oauth_stop_server(void) {
    g_oauth_done = 1;
    if (g_oauth_fd >= 0) {
        /* shutdown で accept を中断し、サーバスレッド側で close する */
        shutdown(g_oauth_fd, SHUT_RDWR);
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