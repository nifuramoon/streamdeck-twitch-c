#include "auto_fix.h"
#include "log.h"
#include "config.h"
#include "oauth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_ATTEMPTS 5
#define LOG_DIR "/.cache/streamdeck-twitch/logs/"

static inline void safe_strcpy(char *dst, size_t size, const char *src) {
    if (!src || size == 0) return;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void save_attempt_log(int attempt, const char *content) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char dir[1024];
    int n = snprintf(dir, sizeof(dir), "%s%s", home, LOG_DIR);
    if (n < 0 || (size_t)n >= sizeof(dir)) return;

    if (ensure_dir(dir) < 0) return;

    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &lt);

    /* attempt log */
    char path[2048];
    n = snprintf(path, sizeof(path), "%sattempt_%d_%s.log", dir, attempt, ts);
    if (n >= 0 && (size_t)n < sizeof(path)) {
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", content); fclose(f); }
    }

    /* latest.log */
    n = snprintf(path, sizeof(path), "%slatest.log", dir);
    if (n >= 0 && (size_t)n < sizeof(path)) {
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", content); fclose(f); }
    }

    /* fix_history.log */
    n = snprintf(path, sizeof(path), "%sfix_history.log", dir);
    if (n >= 0 && (size_t)n < sizeof(path)) {
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "[%s] attempt %d: %s\n", ts, attempt, content);
            fclose(f);
        }
    }
}

void auto_fix_run(int argc, char **argv) {
    (void)argc; (void)argv;
    infoLog("auto-fix mode enabled");

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        infoLog("auto-fix attempt %d/%d", attempt, MAX_ATTEMPTS);

        char log_buf[64];
        int n = snprintf(log_buf, sizeof(log_buf),
                         "Attempt %d at %ld", attempt, (long)time(NULL));
        if (n > 0 && (size_t)n < sizeof(log_buf))
            save_attempt_log(attempt, log_buf);

        infoLog("checking configuration...");
        int cfg_ok = config_load(&g_config);
        if (cfg_ok < 0) {
            warnLog("no config found");
            if (attempt == 1) {
                infoLog("running initial setup...");
                char cid[MAX_STR] = {0}, cs[MAX_STR] = {0};

                infoLog("Enter Twitch Client ID:");
                if (fgets(cid, sizeof(cid), stdin)) {
                    size_t len = strlen(cid);
                    if (len > 0 && cid[len - 1] == '\n') cid[len - 1] = '\0';
                    safe_strcpy(g_config.client_id, sizeof(g_config.client_id), cid);
                }

                infoLog("Enter Twitch Client Secret:");
                if (fgets(cs, sizeof(cs), stdin)) {
                    size_t len = strlen(cs);
                    if (len > 0 && cs[len - 1] == '\n') cs[len - 1] = '\0';
                    safe_strcpy(g_config.client_secret, sizeof(g_config.client_secret), cs);
                }

                oauth_start_server();
                oauth_open_browser(g_config.client_id);
                config_save(&g_config);
            }
        }

        infoLog("attempt %d complete", attempt);
        if (attempt < MAX_ATTEMPTS)
            sleep(2);
    }

    if (MAX_ATTEMPTS > 0)
        infoLog("auto-fix: max attempts reached, starting normally");
}