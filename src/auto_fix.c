#include "auto_fix.h"
#include "log.h"
#include "config.h"
#include "oauth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <features.h>
#include <bits/types/struct_timespec.h>

#define MAX_ATTEMPTS 5
#define LOG_DIR "/.cache/streamdeck-twitch/logs/"

static void save_attempt_log(const char *content) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s%s", home, LOG_DIR);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    system(cmd);

    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", lt);

    char path[2048];
    snprintf(path, sizeof(path), "%sattempt_%d_%s.log", dir, 1, ts);

    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", content); fclose(f); }

    snprintf(path, sizeof(path), "%slatest.log", dir);
    f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", content); fclose(f); }

    snprintf(path, sizeof(path), "%sfix_history.log", dir);
    f = fopen(path, "a");
    if (f) { fprintf(f, "[%s] attempt %d: %s\n", ts, 1, content); fclose(f); }
}

void auto_fix_run(int argc, char **argv) {
    (void)argc; (void)argv;
    infoLog("auto-fix mode enabled");

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        infoLog("auto-fix attempt %d/%d", attempt, MAX_ATTEMPTS);

        char log_buf[4096] = {0};
        snprintf(log_buf, sizeof(log_buf), "Attempt %d at %ld", attempt, time(NULL));
        save_attempt_log(log_buf);

        infoLog("checking configuration...");
        if (config_load(&g_config) < 0) {
            warnLog("no config found");
            if (attempt == 1) {
                infoLog("running initial setup...");
                char cid[MAX_STR], cs[MAX_STR];
                infoLog("Enter Twitch Client ID:");
                if (fgets(cid, MAX_STR, stdin)) {
                    cid[strcspn(cid, "\n")] = 0;
                    strncpy(g_config.client_id, cid, MAX_STR-1);
                }
                infoLog("Enter Twitch Client Secret:");
                if (fgets(cs, MAX_STR, stdin)) {
                    cs[strcspn(cs, "\n")] = 0;
                    strncpy(g_config.client_secret, cs, MAX_STR-1);
                }
                oauth_start_server();
                oauth_open_browser(g_config.client_id);
                config_save(&g_config);
            }
        }

        infoLog("attempt %d complete", attempt);
        if (attempt < MAX_ATTEMPTS) {
            struct timespec ts = { .tv_sec = 2 };
            nanosleep(&ts, NULL);
        }
    }

    if (MAX_ATTEMPTS > 0) {
        infoLog("auto-fix: max attempts reached, starting normally");
    }
}
