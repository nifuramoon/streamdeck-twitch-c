#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

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

static void ensure_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "mkdir -p %s", path);
    system(tmp);
}

void log_init(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(log_path, sizeof(log_path), "%s%s", home, LOG_DIR);
    ensure_dir(log_path);
    char full[1024];
    snprintf(full, sizeof(full), "%s%s", log_path, LOG_FILE);
    log_fp = fopen(full, "a");
}

void log_close(void) {
    if (log_fp) fclose(log_fp);
}

void log_write(enum log_level lvl, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char ts[64];
    strftime(ts, sizeof(ts), "%H:%M:%S", lt);

    fprintf(stderr, "[%s][%s] ", ts, level_str[lvl]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    if (log_fp) {
        fprintf(log_fp, "[%s][%s] ", ts, level_str[lvl]);
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        va_end(ap);
        fprintf(log_fp, "\n");
        fflush(log_fp);
    }
}
