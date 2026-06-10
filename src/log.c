#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define LOG_DIR "/.cache/streamdeck-twitch/logs/"
#define LOG_FILE "latest.log"

bool debug_mode = false;

static FILE *log_fp = NULL;
static char log_path[1024];

static const char *level_str[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO]  = "INFO",
    [LOG_WARN]  = "WARN",
    [LOG_ERROR] = "ERROR"
};

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

void log_init(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    int n = snprintf(log_path, sizeof(log_path), "%s%s", home, LOG_DIR);
    if (n < 0 || (size_t)n >= sizeof(log_path)) {
        log_path[0] = '\0';
        return;
    }

    if (ensure_dir(log_path) < 0) {
        log_path[0] = '\0';
        return;
    }

    char full[1024];
    n = snprintf(full, sizeof(full), "%s%s", log_path, LOG_FILE);
    if (n < 0 || (size_t)n >= sizeof(full)) return;

    log_fp = fopen(full, "a");
    if (!log_fp) log_path[0] = '\0';
}

void log_close(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void log_write(enum log_level lvl, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    char ts[64];
    strftime(ts, sizeof(ts), "%H:%M:%S", &lt);

    const char *lv = level_str[lvl];

    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

    fprintf(stderr, "[%s][%s] ", ts, lv);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");

    if (log_fp) {
        fprintf(log_fp, "[%s][%s] ", ts, lv);
        vfprintf(log_fp, fmt, ap2);
        fprintf(log_fp, "\n");
        fflush(log_fp);
    }

    va_end(ap);
    va_end(ap2);
}